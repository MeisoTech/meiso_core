#include <meiso/core/state.h>

#include "meiso_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct MeisoStatePublisher {
    MeisoResourceDescriptor descriptor;
    MeisoStateConfig config;
    MeisoOwnerScope owner;
    uint64_t version;
    size_t payload_size;
    MeisoDeliveryGeneration delivery;
    MeisoStatePublisherStats stats;
    bool has_value;
    bool delivery_active;
    bool pending;
    bool delivery_accepted;
    uint8_t payload[];
};

struct MeisoStateReplica {
    MeisoResourceDescriptor descriptor;
    MeisoOwnerScope owner;
    uint64_t version;
    size_t payload_size;
    MeisoStateReplicaStats stats;
    bool owner_active;
    bool has_value;
    uint8_t payload[];
};

static bool state_descriptor_valid(const MeisoResourceDescriptor *descriptor)
{
    return meiso_resource_descriptor_validate(descriptor) == MEISO_DESCRIPTOR_VALID &&
        descriptor->kind == MEISO_RESOURCE_KIND_STATE;
}

static bool state_config_valid(const MeisoStateConfig *config)
{
    return config != NULL &&
        (config->delivery_mode == MEISO_STATE_DELIVERY_BEST_EFFORT ||
         config->delivery_mode == MEISO_STATE_DELIVERY_CONVERGENT);
}

static bool arena_aligned(const void *arena)
{
    return arena != NULL &&
        (uintptr_t)arena % meiso_state_alignment_required() == 0U;
}

static bool output_overlaps_arena(
    const void *arena,
    size_t arena_size,
    const void *output,
    size_t output_size
)
{
    const uintptr_t arena_start = (uintptr_t)arena;
    const uintptr_t output_start = (uintptr_t)output;
    uintptr_t arena_end;
    uintptr_t output_end;

    if (arena_size > UINTPTR_MAX - arena_start ||
        output_size > UINTPTR_MAX - output_start) {
        return true;
    }
    arena_end = arena_start + arena_size;
    output_end = output_start + output_size;
    return arena_start < output_end && output_start < arena_end;
}

static bool payload_equal(
    const uint8_t *stored,
    size_t stored_size,
    MeisoByteView candidate
)
{
    return stored_size == candidate.size &&
        (stored_size == 0U || memcmp(stored, candidate.data, stored_size) == 0);
}

static void copy_payload(uint8_t *target, MeisoByteView source)
{
    if (source.size != 0U) {
        memmove(target, source.data, source.size);
    }
}

static bool publisher_valid(const MeisoStatePublisher *publisher)
{
    return publisher != NULL && state_descriptor_valid(&publisher->descriptor) &&
        state_config_valid(&publisher->config);
}

static bool replica_valid(const MeisoStateReplica *replica)
{
    return replica != NULL && state_descriptor_valid(&replica->descriptor);
}

static void publisher_view(
    const MeisoStatePublisher *publisher,
    MeisoStateView *out_view
)
{
    out_view->owner = publisher->owner;
    out_view->version = publisher->version;
    out_view->payload.data = publisher->payload;
    out_view->payload.size = publisher->payload_size;
}

static void replica_view(const MeisoStateReplica *replica, MeisoStateView *out_view)
{
    out_view->owner = replica->owner;
    out_view->version = replica->version;
    out_view->payload.data = replica->payload;
    out_view->payload.size = replica->payload_size;
}

size_t meiso_state_alignment_required(void)
{
    return _Alignof(max_align_t);
}

size_t meiso_state_publisher_memory_required(
    const MeisoResourceDescriptor *descriptor,
    const MeisoStateConfig *config
)
{
    if (!state_descriptor_valid(descriptor) || !state_config_valid(config) ||
        descriptor->max_payload_size > SIZE_MAX - sizeof(MeisoStatePublisher)) {
        return 0U;
    }
    return sizeof(MeisoStatePublisher) + descriptor->max_payload_size;
}

MeisoStateInitStatus meiso_state_publisher_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    const MeisoStateConfig *config,
    MeisoStatePublisher **out_publisher
)
{
    size_t required;
    MeisoStatePublisher *publisher;
    MeisoResourceDescriptor descriptor_copy;
    MeisoStateConfig config_copy;

    if (out_publisher == NULL) {
        return MEISO_STATE_INIT_INVALID_ARGUMENT;
    }
    if (arena != NULL && output_overlaps_arena(
        arena, arena_size, out_publisher, sizeof(*out_publisher)
    )) {
        return MEISO_STATE_INIT_INVALID_ARGUMENT;
    }
    *out_publisher = NULL;
    if (!state_descriptor_valid(descriptor)) {
        return MEISO_STATE_INIT_INVALID_DESCRIPTOR;
    }
    if (!state_config_valid(config)) {
        return MEISO_STATE_INIT_INVALID_CONFIG;
    }
    if (!arena_aligned(arena)) {
        return MEISO_STATE_INIT_MISALIGNED_ARENA;
    }
    required = meiso_state_publisher_memory_required(descriptor, config);
    if (required == 0U || arena_size < required) {
        return MEISO_STATE_INIT_ARENA_TOO_SMALL;
    }
    descriptor_copy = *descriptor;
    config_copy = *config;
    memset(arena, 0, required);
    publisher = arena;
    publisher->descriptor = descriptor_copy;
    publisher->config = config_copy;
    *out_publisher = publisher;
    return MEISO_STATE_INIT_OK;
}

MeisoStatePublishStatus meiso_state_publisher_publish(
    MeisoStatePublisher *publisher,
    MeisoOwnerScope owner,
    uint64_t version,
    MeisoByteView payload
)
{
    bool replaced_pending;

    if (!publisher_valid(publisher) || !meiso_owner_scope_valid(owner) ||
        version == 0U || !meiso_byte_view_valid(payload)) {
        return MEISO_STATE_PUBLISH_INVALID;
    }
    if (owner.role != publisher->descriptor.owner ||
        (publisher->has_value && !meiso_owner_scope_equal(publisher->owner, owner))) {
        meiso_increment_saturated(&publisher->stats.scope_mismatches);
        return MEISO_STATE_PUBLISH_SCOPE_MISMATCH;
    }
    if (publisher->has_value && version == publisher->version) {
        if (payload_equal(publisher->payload, publisher->payload_size, payload)) {
            meiso_increment_saturated(&publisher->stats.duplicates);
            return MEISO_STATE_PUBLISH_DUPLICATE;
        }
        meiso_increment_saturated(&publisher->stats.conflicts);
        return MEISO_STATE_PUBLISH_CONFLICT;
    }
    if (publisher->has_value && version < publisher->version) {
        if (publisher->version == UINT64_MAX) {
            return MEISO_STATE_PUBLISH_VERSION_WRAP;
        }
        meiso_increment_saturated(&publisher->stats.stale);
        return MEISO_STATE_PUBLISH_STALE;
    }
    if (payload.size > publisher->descriptor.max_payload_size) {
        meiso_increment_saturated(&publisher->stats.capacity_failures);
        return MEISO_STATE_PUBLISH_CAPACITY;
    }

    replaced_pending = publisher->pending;
    copy_payload(publisher->payload, payload);
    publisher->owner = owner;
    publisher->version = version;
    publisher->payload_size = payload.size;
    publisher->has_value = true;
    publisher->pending = publisher->delivery_active;
    publisher->delivery_accepted = false;
    meiso_increment_saturated(&publisher->stats.publishes);
    if (replaced_pending) {
        meiso_increment_saturated(&publisher->stats.replaced_pending);
        return MEISO_STATE_PUBLISH_REPLACED_PENDING;
    }
    return MEISO_STATE_PUBLISH_NEW;
}

MeisoStateDeliveryBeginStatus meiso_state_publisher_begin_delivery(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery
)
{
    if (!publisher_valid(publisher) || !meiso_delivery_generation_valid(delivery)) {
        return MEISO_STATE_DELIVERY_INVALID;
    }
    if (publisher->delivery_active) {
        return MEISO_STATE_DELIVERY_ALREADY_ACTIVE;
    }
    if (publisher->delivery.value != 0U && delivery.value <= publisher->delivery.value) {
        if (publisher->delivery.value == UINT64_MAX) {
            return MEISO_STATE_DELIVERY_GENERATION_WRAP;
        }
        return MEISO_STATE_DELIVERY_STALE_GENERATION;
    }
    publisher->delivery = delivery;
    publisher->delivery_active = true;
    publisher->pending = publisher->has_value;
    publisher->delivery_accepted = false;
    return MEISO_STATE_DELIVERY_STARTED;
}

bool meiso_state_publisher_current(
    const MeisoStatePublisher *publisher,
    MeisoStateView *out_view
)
{
    if (!publisher_valid(publisher) || out_view == NULL || !publisher->has_value) {
        return false;
    }
    publisher_view(publisher, out_view);
    return true;
}

bool meiso_state_publisher_next_offer(
    const MeisoStatePublisher *publisher,
    MeisoStateOffer *out_offer
)
{
    if (!publisher_valid(publisher) || out_offer == NULL ||
        !publisher->delivery_active || !publisher->pending) {
        return false;
    }
    out_offer->generation = publisher->delivery;
    publisher_view(publisher, &out_offer->state);
    return true;
}

MeisoStateAcceptStatus meiso_state_publisher_mark_delivery_accepted(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    if (!publisher_valid(publisher) || !meiso_delivery_generation_valid(delivery) ||
        !meiso_owner_scope_valid(owner) || version == 0U) {
        return MEISO_STATE_ACCEPT_INVALID;
    }
    if (!publisher->delivery_active) {
        return MEISO_STATE_ACCEPT_NOT_ACTIVE;
    }
    if (delivery.value != publisher->delivery.value) {
        return MEISO_STATE_ACCEPT_DELIVERY_MISMATCH;
    }
    if (!publisher->has_value) {
        return MEISO_STATE_ACCEPT_NOT_PENDING;
    }
    if (!meiso_owner_scope_equal(owner, publisher->owner)) {
        meiso_increment_saturated(&publisher->stats.scope_mismatches);
        return MEISO_STATE_ACCEPT_SCOPE_MISMATCH;
    }
    if (version < publisher->version) {
        return MEISO_STATE_ACCEPT_STALE;
    }
    if (version > publisher->version) {
        return MEISO_STATE_ACCEPT_FUTURE;
    }
    if (publisher->delivery_accepted) {
        return MEISO_STATE_ACCEPT_DUPLICATE;
    }
    publisher->delivery_accepted = true;
    publisher->pending = false;
    meiso_increment_saturated(&publisher->stats.delivery_accepts);
    return MEISO_STATE_ACCEPT_CONFIRMED;
}

MeisoStateDeliveryFailureStatus meiso_state_publisher_mark_delivery_failed(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    if (!publisher_valid(publisher) || !meiso_delivery_generation_valid(delivery) ||
        !meiso_owner_scope_valid(owner) || version == 0U) {
        return MEISO_STATE_FAILURE_INVALID;
    }
    if (!publisher->delivery_active) {
        return MEISO_STATE_FAILURE_NOT_ACTIVE;
    }
    if (delivery.value != publisher->delivery.value) {
        return MEISO_STATE_FAILURE_DELIVERY_MISMATCH;
    }
    if (!publisher->has_value) {
        return MEISO_STATE_FAILURE_NOT_PENDING;
    }
    if (!meiso_owner_scope_equal(owner, publisher->owner)) {
        meiso_increment_saturated(&publisher->stats.scope_mismatches);
        return MEISO_STATE_FAILURE_SCOPE_MISMATCH;
    }
    if (version != publisher->version || !publisher->pending) {
        return MEISO_STATE_FAILURE_NOT_PENDING;
    }
    meiso_increment_saturated(&publisher->stats.delivery_failures);
    if (publisher->config.delivery_mode == MEISO_STATE_DELIVERY_BEST_EFFORT) {
        publisher->pending = false;
        meiso_increment_saturated(&publisher->stats.best_effort_drops);
        return MEISO_STATE_FAILURE_DROPPED;
    }
    return MEISO_STATE_FAILURE_RETAINED;
}

MeisoStateOfferCompleteStatus meiso_state_publisher_mark_offer_complete(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery,
    MeisoOwnerScope owner,
    uint64_t version
)
{
    if (!publisher_valid(publisher) || !meiso_delivery_generation_valid(delivery) ||
        !meiso_owner_scope_valid(owner) || version == 0U) {
        return MEISO_STATE_OFFER_COMPLETE_INVALID;
    }
    if (publisher->config.delivery_mode != MEISO_STATE_DELIVERY_BEST_EFFORT) {
        return MEISO_STATE_OFFER_COMPLETE_WRONG_MODE;
    }
    if (!publisher->delivery_active) {
        return MEISO_STATE_OFFER_COMPLETE_NOT_ACTIVE;
    }
    if (delivery.value != publisher->delivery.value) {
        return MEISO_STATE_OFFER_COMPLETE_DELIVERY_MISMATCH;
    }
    if (!publisher->has_value) {
        return MEISO_STATE_OFFER_COMPLETE_NOT_PENDING;
    }
    if (!meiso_owner_scope_equal(owner, publisher->owner)) {
        meiso_increment_saturated(&publisher->stats.scope_mismatches);
        return MEISO_STATE_OFFER_COMPLETE_SCOPE_MISMATCH;
    }
    if (version != publisher->version || !publisher->pending) {
        return MEISO_STATE_OFFER_COMPLETE_NOT_PENDING;
    }
    publisher->pending = false;
    meiso_increment_saturated(&publisher->stats.best_effort_offer_completions);
    return MEISO_STATE_OFFER_COMPLETED;
}

MeisoStateDeliveryEndStatus meiso_state_publisher_end_delivery(
    MeisoStatePublisher *publisher,
    MeisoDeliveryGeneration delivery
)
{
    MeisoStateDeliveryEndStatus status = MEISO_STATE_DELIVERY_ENDED;

    if (!publisher_valid(publisher) || !meiso_delivery_generation_valid(delivery)) {
        return MEISO_STATE_DELIVERY_END_INVALID;
    }
    if (!publisher->delivery_active) {
        return MEISO_STATE_DELIVERY_END_NOT_ACTIVE;
    }
    if (delivery.value != publisher->delivery.value) {
        return MEISO_STATE_DELIVERY_END_MISMATCH;
    }
    if (publisher->pending) {
        if (publisher->config.delivery_mode == MEISO_STATE_DELIVERY_CONVERGENT) {
            meiso_increment_saturated(&publisher->stats.convergence_failures);
            status = MEISO_STATE_DELIVERY_CONVERGENCE_FAILED;
        } else {
            publisher->pending = false;
            meiso_increment_saturated(&publisher->stats.best_effort_drops);
            status = MEISO_STATE_DELIVERY_ENDED_WITH_DROP;
        }
    }
    publisher->pending = false;
    publisher->delivery_accepted = false;
    publisher->delivery_active = false;
    return status;
}

bool meiso_state_publisher_stats(
    const MeisoStatePublisher *publisher,
    MeisoStatePublisherStats *out_stats
)
{
    if (!publisher_valid(publisher) || out_stats == NULL) {
        return false;
    }
    *out_stats = publisher->stats;
    return true;
}

size_t meiso_state_replica_memory_required(
    const MeisoResourceDescriptor *descriptor
)
{
    if (!state_descriptor_valid(descriptor) ||
        descriptor->max_payload_size > SIZE_MAX - sizeof(MeisoStateReplica)) {
        return 0U;
    }
    return sizeof(MeisoStateReplica) + descriptor->max_payload_size;
}

MeisoStateInitStatus meiso_state_replica_init(
    void *arena,
    size_t arena_size,
    const MeisoResourceDescriptor *descriptor,
    MeisoStateReplica **out_replica
)
{
    size_t required;
    MeisoStateReplica *replica;
    MeisoResourceDescriptor descriptor_copy;

    if (out_replica == NULL) {
        return MEISO_STATE_INIT_INVALID_ARGUMENT;
    }
    if (arena != NULL && output_overlaps_arena(
        arena, arena_size, out_replica, sizeof(*out_replica)
    )) {
        return MEISO_STATE_INIT_INVALID_ARGUMENT;
    }
    *out_replica = NULL;
    if (!state_descriptor_valid(descriptor)) {
        return MEISO_STATE_INIT_INVALID_DESCRIPTOR;
    }
    if (!arena_aligned(arena)) {
        return MEISO_STATE_INIT_MISALIGNED_ARENA;
    }
    required = meiso_state_replica_memory_required(descriptor);
    if (required == 0U || arena_size < required) {
        return MEISO_STATE_INIT_ARENA_TOO_SMALL;
    }
    descriptor_copy = *descriptor;
    memset(arena, 0, required);
    replica = arena;
    replica->descriptor = descriptor_copy;
    *out_replica = replica;
    return MEISO_STATE_INIT_OK;
}

MeisoStateOwnerStatus meiso_state_replica_begin_owner(
    MeisoStateReplica *replica,
    MeisoOwnerScope owner
)
{
    if (!replica_valid(replica) || !meiso_owner_scope_valid(owner)) {
        return MEISO_STATE_OWNER_INVALID;
    }
    if (owner.role != replica->descriptor.owner) {
        return MEISO_STATE_OWNER_SCOPE_MISMATCH;
    }
    if (replica->owner_active && meiso_owner_scope_equal(replica->owner, owner)) {
        return MEISO_STATE_OWNER_UNCHANGED;
    }
    replica->owner = owner;
    replica->owner_active = true;
    replica->has_value = false;
    replica->version = 0U;
    replica->payload_size = 0U;
    return MEISO_STATE_OWNER_STARTED;
}

MeisoStateReceiveStatus meiso_state_replica_receive(
    MeisoStateReplica *replica,
    MeisoOwnerScope owner,
    uint64_t version,
    MeisoByteView payload
)
{
    if (!replica_valid(replica) || !meiso_owner_scope_valid(owner) ||
        version == 0U || !meiso_byte_view_valid(payload)) {
        return MEISO_STATE_RECEIVE_INVALID;
    }
    if (owner.role != replica->descriptor.owner) {
        meiso_increment_saturated(&replica->stats.scope_mismatches);
        return MEISO_STATE_RECEIVE_SCOPE_MISMATCH;
    }
    if (!replica->owner_active) {
        return MEISO_STATE_RECEIVE_NO_OWNER;
    }
    if (!meiso_owner_scope_equal(replica->owner, owner)) {
        meiso_increment_saturated(&replica->stats.scope_mismatches);
        return MEISO_STATE_RECEIVE_SCOPE_MISMATCH;
    }
    if (replica->has_value && version == replica->version) {
        if (payload_equal(replica->payload, replica->payload_size, payload)) {
            meiso_increment_saturated(&replica->stats.duplicates);
            return MEISO_STATE_RECEIVE_DUPLICATE;
        }
        meiso_increment_saturated(&replica->stats.conflicts);
        return MEISO_STATE_RECEIVE_CONFLICT;
    }
    if (replica->has_value && version < replica->version) {
        if (replica->version == UINT64_MAX) {
            return MEISO_STATE_RECEIVE_VERSION_WRAP;
        }
        meiso_increment_saturated(&replica->stats.stale);
        return MEISO_STATE_RECEIVE_STALE;
    }
    if (payload.size > replica->descriptor.max_payload_size) {
        meiso_increment_saturated(&replica->stats.capacity_failures);
        return MEISO_STATE_RECEIVE_CAPACITY;
    }
    copy_payload(replica->payload, payload);
    replica->version = version;
    replica->payload_size = payload.size;
    replica->has_value = true;
    meiso_increment_saturated(&replica->stats.receives);
    return MEISO_STATE_RECEIVE_NEW;
}

bool meiso_state_replica_current(
    const MeisoStateReplica *replica,
    MeisoStateView *out_view
)
{
    if (!replica_valid(replica) || out_view == NULL || !replica->has_value) {
        return false;
    }
    replica_view(replica, out_view);
    return true;
}

bool meiso_state_replica_stats(
    const MeisoStateReplica *replica,
    MeisoStateReplicaStats *out_stats
)
{
    if (!replica_valid(replica) || out_stats == NULL) {
        return false;
    }
    *out_stats = replica->stats;
    return true;
}
