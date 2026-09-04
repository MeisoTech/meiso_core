#include "meiso_runtime_private.h"

#include <string.h>

/* Inline Stream carriage and production-gate enforcement. */

static bool stream_sample_valid(
    const MeisoStreamSample *sample,
    const MeisoResourceDescriptor *resource
)
{
    return sample != NULL && resource != NULL &&
        meiso_owner_scope_valid(sample->key.producer) &&
        sample->key.epoch != 0U && sample->key.sequence != 0U &&
        meiso_time_point_valid(sample->source_time) &&
        meiso_deadline_valid(sample->sender_deadline) &&
        (!sample->sender_deadline.present ||
         sample->sender_deadline.value.clock_epoch ==
            sample->source_time.clock_epoch) &&
        meiso_byte_view_valid(sample->dependency) &&
        meiso_byte_view_valid(sample->payload) &&
        sample->dependency.size <= UINT32_MAX &&
        sample->payload.size <= resource->max_payload_size &&
        sample->payload.size <= UINT32_MAX;
}

MeisoRuntimeStatus meiso_runtime_send_stream_sample(
    MeisoRuntime *runtime,
    MeisoRuntimeRouteKey route_key,
    uint64_t production_epoch,
    const MeisoStreamSample *sample
)
{
    MeisoRuntimeStatus status;
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeEndpointSlot *endpoint;
    MeisoOwnerRole producer_role;
    uint8_t *output;
    size_t size;

    if (meiso_runtime_private_layout_valid(runtime) &&
        runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 &&
        meiso_runtime_private_check_bound_const(runtime) == MEISO_RUNTIME_OK) {
        const MeisoRuntimeRouteSlot *const_route;
        const MeisoRuntimeEndpointSlot *const_endpoint;
        size_t route_index;

        status = meiso_runtime_private_resolve_active_route_const(
            runtime, route_key, &const_route, &const_endpoint, &route_index
        );
        (void)const_route;
        (void)const_endpoint;
        if (status == MEISO_RUNTIME_OK && meiso_runtime_private_route_delivery_at(
                runtime, runtime->active_bank, route_index
            ) == MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA) {
            return MEISO_RUNTIME_DELIVERY_MISMATCH;
        }
    }
    status = meiso_runtime_private_check_bound_mutable(runtime);

    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    meiso_runtime_private_clear_pending_state_receive(runtime);
    status = meiso_runtime_private_resolve_active_route(runtime, route_key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (route->value.facet != MEISO_RUNTIME_FACET_DATA ||
        route->value.resource.kind != MEISO_RESOURCE_KIND_STREAM) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    producer_role = runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 ?
        route->value.resource.owner : MEISO_OWNER_ROLE_EDGE;
    if (meiso_runtime_private_owner_role_for_session(runtime->local_role) !=
            producer_role) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!stream_sample_valid(sample, &route->value.resource)) {
        return MEISO_RUNTIME_STREAM_INVALID;
    }
    if (meiso_runtime_private_ranges_overlap(
            runtime,
            runtime->arena_size,
            sample->dependency.data,
            sample->dependency.size
        ) || meiso_runtime_private_ranges_overlap(
            runtime,
            runtime->arena_size,
            sample->payload.data,
            sample->payload.size
        )) {
        return MEISO_RUNTIME_INVALID_ARGUMENT;
    }
    if (sample->key.producer.role != producer_role) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (!endpoint->production.data_active) {
        return MEISO_RUNTIME_ROUTE_NOT_ACTIVE;
    }
    if (production_epoch == 0U ||
        production_epoch != endpoint->production.production_epoch) {
        return MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH;
    }
    if (!meiso_runtime_private_checked_add(
            MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE,
            sample->dependency.size,
            &size
        ) || !meiso_runtime_private_checked_add(size, sample->payload.size, &size) ||
        size > runtime->limits.max_runtime_payload_bytes || size > UINT32_MAX) {
        return MEISO_RUNTIME_PAYLOAD_CAPACITY;
    }
    output = meiso_runtime_private_tx_bytes(runtime);
    meiso_runtime_private_encode_route_key(output + MEISO_RUNTIME_WIRE_HEADER_SIZE, route_key);
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 56U,
        production_epoch
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 64U] =
        sample->key.producer.role;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 65U, 0, 7U);
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 72U,
        sample->key.producer.incarnation
    );
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 80U,
        sample->key.epoch
    );
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 88U,
        sample->key.sequence
    );
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 96U,
        sample->source_time.clock_epoch
    );
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 104U,
        sample->source_time.monotonic_ns
    );
    output[MEISO_RUNTIME_WIRE_HEADER_SIZE + 112U] =
        sample->sender_deadline.present ? 1U : 0U;
    memset(output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 113U, 0, 7U);
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 120U,
        sample->sender_deadline.present ?
            sample->sender_deadline.value.clock_epoch : 0U
    );
    meiso_runtime_private_put_u64(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 128U,
        sample->sender_deadline.present ?
            sample->sender_deadline.value.monotonic_ns : 0U
    );
    meiso_runtime_private_put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 136U,
        (uint32_t)sample->dependency.size
    );
    meiso_runtime_private_put_u32(
        output + MEISO_RUNTIME_WIRE_HEADER_SIZE + 140U,
        (uint32_t)sample->payload.size
    );
    if (sample->dependency.size != 0U) {
        memmove(
            output + MEISO_RUNTIME_WIRE_HEADER_SIZE +
                MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE,
            sample->dependency.data,
            sample->dependency.size
        );
    }
    if (sample->payload.size != 0U) {
        memmove(
            output + MEISO_RUNTIME_WIRE_HEADER_SIZE +
                MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE + sample->dependency.size,
            sample->payload.data,
            sample->payload.size
        );
    }
    meiso_runtime_private_encode_header(runtime, output, MEISO_RUNTIME_WIRE_STREAM_SAMPLE, (uint32_t)size);
    return meiso_runtime_private_submit_bytes(runtime, size);
}
MeisoRuntimeStatus meiso_runtime_private_receive_stream_sample(
    MeisoRuntime *runtime,
    MeisoByteView frame,
    MeisoRuntimeEvent *event
)
{
    const uint8_t *body = frame.data + MEISO_RUNTIME_WIRE_HEADER_SIZE;
    MeisoRuntimeRouteKey key;
    MeisoStreamSample sample;
    uint64_t production_epoch;
    uint32_t dependency_size;
    uint32_t payload_size;
    size_t variable_size;
    MeisoRuntimeRouteSlot *route;
    MeisoRuntimeEndpointSlot *endpoint;
    MeisoRuntimeStatus status;
    MeisoOwnerRole producer_role;

    if (frame.size <
            MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE ||
        !meiso_runtime_private_decode_route_key(body, &key) ||
        !meiso_runtime_private_bytes_zero(body + 65U, 7U) ||
        body[112U] > 1U || !meiso_runtime_private_bytes_zero(body + 113U, 7U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    dependency_size = meiso_runtime_private_get_u32(body + 136U);
    payload_size = meiso_runtime_private_get_u32(body + 140U);
    if (!meiso_runtime_private_checked_add(dependency_size, payload_size, &variable_size) ||
        variable_size != frame.size -
            (MEISO_RUNTIME_WIRE_HEADER_SIZE + MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    production_epoch = meiso_runtime_private_get_u64(body + 56U);
    memset(&sample, 0, sizeof(sample));
    sample.key.producer.role = body[64U];
    sample.key.producer.incarnation = meiso_runtime_private_get_u64(body + 72U);
    sample.key.epoch = meiso_runtime_private_get_u64(body + 80U);
    sample.key.sequence = meiso_runtime_private_get_u64(body + 88U);
    sample.source_time.clock_epoch = meiso_runtime_private_get_u64(body + 96U);
    sample.source_time.monotonic_ns = meiso_runtime_private_get_u64(body + 104U);
    sample.sender_deadline.present = body[112U] != 0U;
    sample.sender_deadline.value.clock_epoch = meiso_runtime_private_get_u64(body + 120U);
    sample.sender_deadline.value.monotonic_ns = meiso_runtime_private_get_u64(body + 128U);
    if (!sample.sender_deadline.present &&
        (sample.sender_deadline.value.clock_epoch != 0U ||
         sample.sender_deadline.value.monotonic_ns != 0U)) {
        return MEISO_RUNTIME_MALFORMED;
    }
    sample.dependency.data = body + MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE;
    sample.dependency.size = dependency_size;
    sample.payload.data = body + MEISO_RUNTIME_STREAM_SAMPLE_PREFIX_SIZE + dependency_size;
    sample.payload.size = payload_size;

    status = meiso_runtime_private_resolve_active_route(runtime, key, &route, &endpoint);
    if (status != MEISO_RUNTIME_OK) {
        return status;
    }
    if (meiso_runtime_private_route_delivery_at(
            runtime,
            runtime->active_bank,
            (size_t)(route - meiso_runtime_private_route_slots(runtime, runtime->active_bank))
        ) == MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA) {
        return MEISO_RUNTIME_DELIVERY_MISMATCH;
    }
    if (route->value.facet != MEISO_RUNTIME_FACET_DATA ||
        route->value.resource.kind != MEISO_RESOURCE_KIND_STREAM) {
        return MEISO_RUNTIME_UNSUPPORTED_KIND;
    }
    producer_role = runtime->wire_version == MEISO_RUNTIME_WIRE_VERSION_V4 ?
        route->value.resource.owner : MEISO_OWNER_ROLE_EDGE;
    if (meiso_runtime_private_owner_role_for_session(runtime->local_role) ==
            producer_role) {
        return MEISO_RUNTIME_WRONG_ROLE;
    }
    if (!stream_sample_valid(&sample, &route->value.resource)) {
        return MEISO_RUNTIME_STREAM_INVALID;
    }
    if (sample.key.producer.role != producer_role) {
        return MEISO_RUNTIME_OWNER_MISMATCH;
    }
    if (!endpoint->production.data_active) {
        return MEISO_RUNTIME_ROUTE_NOT_ACTIVE;
    }
    if (production_epoch == 0U ||
        production_epoch != endpoint->production.production_epoch) {
        return MEISO_RUNTIME_PRODUCTION_EPOCH_MISMATCH;
    }
    event->type = MEISO_RUNTIME_EVENT_STREAM_SAMPLE;
    event->catalog_generation = key.catalog_generation;
    event->route = key;
    event->production_epoch = production_epoch;
    event->stream_sample = sample;
    return MEISO_RUNTIME_OK;
}
