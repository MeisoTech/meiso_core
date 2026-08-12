#ifndef MEISO_STATE_H
#define MEISO_STATE_H

#include "meiso_resource.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MeisoStatePublisher MeisoStatePublisher;
typedef struct MeisoStateReplica MeisoStateReplica;

typedef uint8_t MeisoStateDeliveryMode;
enum {
    MEISO_STATE_DELIVERY_BEST_EFFORT = 1,
    MEISO_STATE_DELIVERY_CONVERGENT = 2
};

typedef struct {
    MeisoStateDeliveryMode delivery_mode;
} MeisoStateConfig;

typedef struct {
    MeisoOwnerScope owner;
    uint64_t version;
    MeisoByteView payload;
} MeisoStateView;

typedef struct {
    MeisoDeliveryGeneration generation;
    MeisoStateView state;
} MeisoStateOffer;

typedef struct {
    uint64_t publishes;
    uint64_t replaced_pending;
    uint64_t duplicates;
    uint64_t stale;
    uint64_t conflicts;
    uint64_t capacity_failures;
    uint64_t scope_mismatches;
    uint64_t delivery_accepts;
    uint64_t delivery_failures;
    uint64_t best_effort_offer_completions;
    uint64_t best_effort_drops;
    uint64_t convergence_failures;
} MeisoStatePublisherStats;

typedef struct {
    uint64_t receives;
    uint64_t duplicates;
    uint64_t stale;
    uint64_t conflicts;
    uint64_t capacity_failures;
    uint64_t scope_mismatches;
} MeisoStateReplicaStats;

typedef uint8_t MeisoStateInitStatus;
enum {
    MEISO_STATE_INIT_OK = 0,
    MEISO_STATE_INIT_INVALID_ARGUMENT = 1,
    MEISO_STATE_INIT_INVALID_DESCRIPTOR = 2,
    MEISO_STATE_INIT_INVALID_CONFIG = 3,
    MEISO_STATE_INIT_MISALIGNED_ARENA = 4,
    MEISO_STATE_INIT_ARENA_TOO_SMALL = 5
};

typedef uint8_t MeisoStatePublishStatus;
enum {
    MEISO_STATE_PUBLISH_NEW = 0,
    MEISO_STATE_PUBLISH_REPLACED_PENDING = 1,
    MEISO_STATE_PUBLISH_DUPLICATE = 2,
    MEISO_STATE_PUBLISH_STALE = 3,
    MEISO_STATE_PUBLISH_CONFLICT = 4,
    MEISO_STATE_PUBLISH_VERSION_WRAP = 5,
    MEISO_STATE_PUBLISH_CAPACITY = 6,
    MEISO_STATE_PUBLISH_SCOPE_MISMATCH = 7,
    MEISO_STATE_PUBLISH_INVALID = 8
};

typedef uint8_t MeisoStateDeliveryBeginStatus;
enum {
    MEISO_STATE_DELIVERY_STARTED = 0,
    MEISO_STATE_DELIVERY_ALREADY_ACTIVE = 1,
    MEISO_STATE_DELIVERY_STALE_GENERATION = 2,
    MEISO_STATE_DELIVERY_GENERATION_WRAP = 3,
    MEISO_STATE_DELIVERY_INVALID = 4
};

typedef uint8_t MeisoStateAcceptStatus;
enum {
    MEISO_STATE_ACCEPT_CONFIRMED = 0,
    MEISO_STATE_ACCEPT_DUPLICATE = 1,
    MEISO_STATE_ACCEPT_STALE = 2,
    MEISO_STATE_ACCEPT_FUTURE = 3,
    MEISO_STATE_ACCEPT_SCOPE_MISMATCH = 4,
    MEISO_STATE_ACCEPT_DELIVERY_MISMATCH = 5,
    MEISO_STATE_ACCEPT_NOT_ACTIVE = 6,
    MEISO_STATE_ACCEPT_NOT_PENDING = 7,
    MEISO_STATE_ACCEPT_INVALID = 8
};

typedef uint8_t MeisoStateDeliveryFailureStatus;
enum {
    MEISO_STATE_FAILURE_DROPPED = 0,
    MEISO_STATE_FAILURE_RETAINED = 1,
    MEISO_STATE_FAILURE_NOT_PENDING = 2,
    MEISO_STATE_FAILURE_SCOPE_MISMATCH = 3,
    MEISO_STATE_FAILURE_DELIVERY_MISMATCH = 4,
    MEISO_STATE_FAILURE_NOT_ACTIVE = 5,
    MEISO_STATE_FAILURE_INVALID = 6
};

typedef uint8_t MeisoStateOfferCompleteStatus;
enum {
    MEISO_STATE_OFFER_COMPLETED = 0,
    MEISO_STATE_OFFER_COMPLETE_WRONG_MODE = 1,
    MEISO_STATE_OFFER_COMPLETE_NOT_PENDING = 2,
    MEISO_STATE_OFFER_COMPLETE_SCOPE_MISMATCH = 3,
    MEISO_STATE_OFFER_COMPLETE_DELIVERY_MISMATCH = 4,
    MEISO_STATE_OFFER_COMPLETE_NOT_ACTIVE = 5,
    MEISO_STATE_OFFER_COMPLETE_INVALID = 6
};

typedef uint8_t MeisoStateDeliveryEndStatus;
enum {
    MEISO_STATE_DELIVERY_ENDED = 0,
    MEISO_STATE_DELIVERY_ENDED_WITH_DROP = 1,
    MEISO_STATE_DELIVERY_CONVERGENCE_FAILED = 2,
    MEISO_STATE_DELIVERY_END_MISMATCH = 3,
    MEISO_STATE_DELIVERY_END_NOT_ACTIVE = 4,
    MEISO_STATE_DELIVERY_END_INVALID = 5
};

typedef uint8_t MeisoStateOwnerStatus;
enum {
    MEISO_STATE_OWNER_STARTED = 0,
    MEISO_STATE_OWNER_UNCHANGED = 1,
    MEISO_STATE_OWNER_SCOPE_MISMATCH = 2,
    MEISO_STATE_OWNER_INVALID = 3
};

typedef uint8_t MeisoStateReceiveStatus;
enum {
    MEISO_STATE_RECEIVE_NEW = 0,
    MEISO_STATE_RECEIVE_DUPLICATE = 1,
    MEISO_STATE_RECEIVE_STALE = 2,
    MEISO_STATE_RECEIVE_CONFLICT = 3,
    MEISO_STATE_RECEIVE_VERSION_WRAP = 4,
    MEISO_STATE_RECEIVE_CAPACITY = 5,
    MEISO_STATE_RECEIVE_SCOPE_MISMATCH = 6,
    MEISO_STATE_RECEIVE_NO_OWNER = 7,
    MEISO_STATE_RECEIVE_INVALID = 8
};

size_t meiso_state_alignment_required(void);
size_t meiso_state_publisher_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoStateConfig *config
);
MeisoStateInitStatus meiso_state_publisher_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoStateConfig *config,
    MeisoStatePublisher **out_publisher
);
MeisoStatePublishStatus meiso_state_publisher_publish(
    MeisoStatePublisher *publisher,
    MeisoOwnerScope owner,
    uint64_t version,
    MeisoByteView payload
);
MeisoStateDeliveryBeginStatus meiso_state_publisher_begin_delivery(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery
);
bool meiso_state_publisher_current(
    const MeisoStatePublisher *publisher,
    MeisoStateView *out_view
);
bool meiso_state_publisher_next_offer(
    const MeisoStatePublisher *publisher,
    MeisoStateOffer *out_offer
);
MeisoStateAcceptStatus meiso_state_publisher_mark_delivery_accepted(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery,
    MeisoOwnerScope owner,
    uint64_t version
);
MeisoStateDeliveryFailureStatus meiso_state_publisher_mark_delivery_failed(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery,
    MeisoOwnerScope owner,
    uint64_t version
);
MeisoStateOfferCompleteStatus meiso_state_publisher_mark_offer_complete(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery,
    MeisoOwnerScope owner,
    uint64_t version
);
MeisoStateDeliveryEndStatus meiso_state_publisher_end_delivery(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery
);
bool meiso_state_publisher_stats(
    const MeisoStatePublisher *publisher,
    MeisoStatePublisherStats *out_stats
);

size_t meiso_state_replica_memory_required(
    const MeisoResourceDescriptor *descriptor
);
MeisoStateInitStatus meiso_state_replica_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    MeisoStateReplica **out_replica
);
MeisoStateOwnerStatus meiso_state_replica_begin_owner(
    MeisoStateReplica *replica,
    MeisoOwnerScope owner
);
MeisoStateReceiveStatus meiso_state_replica_receive(
    MeisoStateReplica *replica,
    MeisoOwnerScope owner,
    uint64_t version,
    MeisoByteView payload
);
bool meiso_state_replica_current(
    const MeisoStateReplica *replica,
    MeisoStateView *out_view
);
bool meiso_state_replica_stats(
    const MeisoStateReplica *replica,
    MeisoStateReplicaStats *out_stats
);

#ifdef __cplusplus
}
#endif

#endif
