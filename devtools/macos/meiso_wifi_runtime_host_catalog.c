#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Installs runtime resources and validates/accepts the edge catalog. */

bool meiso_wifi_runtime_host_private_initialize_runtime_resources(
    MeisoWifiRuntimeHostPrivate *host,
    const MeisoSessionSnapshot *session_snapshot
)
{
    MeisoRuntimeConfig runtime_config;
    const MeisoResourceDescriptor *resources;
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    const MeisoStreamConfig stream_config = {
        MEISO_STREAM_ORDER_STRICT_MONOTONIC,
        MEISO_STREAM_OVERFLOW_REJECT_NEWEST,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,
        MEISO_WIFI_RUNTIME_HOST_PRIVATE_STREAM_QUEUE_SLOTS,
        MEISO_WIFI_RUNTIME_HOST_PRIVATE_STREAM_HISTORY_SLOTS,
        MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_STREAM_DEPENDENCY_BYTES
    };
    size_t resource_count = 0U;
    size_t required;

    if (host == NULL || session_snapshot == NULL ||
        session_snapshot->remote_incarnation == 0U) {
        return false;
    }
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.session = host->session;
    runtime_config.max_endpoints = 1U;
    runtime_config.max_endpoint_history = 4U;
    runtime_config.max_routes = MEISO_WIFI_TELEMETRY_RESOURCE_COUNT;
    runtime_config.max_capability_bytes =
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE;
    runtime_config.max_desired_profile_bytes_per_endpoint =
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE;
    runtime_config.max_actual_profile_bytes_per_endpoint =
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE;
    required = meiso_runtime_memory_required(&runtime_config);
    if (required == 0U || required > sizeof(meiso_wifi_runtime_host_private_storage.runtime.bytes) ||
        meiso_runtime_init(
            meiso_wifi_runtime_host_private_storage.runtime.bytes, sizeof(meiso_wifi_runtime_host_private_storage.runtime.bytes),
            &runtime_config, &host->runtime
        ) != MEISO_RUNTIME_INIT_OK) {
        return false;
    }

    resources = meiso_wifi_telemetry_resources(&resource_count);
    if (resources == NULL ||
        resource_count != MEISO_WIFI_TELEMETRY_RESOURCE_COUNT) {
        return false;
    }
    required = meiso_state_publisher_memory_required(
        &resources[0], &state_config
    );
    if (required == 0U ||
        required > sizeof(meiso_wifi_runtime_host_private_storage.desired_publisher.bytes) ||
        meiso_state_publisher_init(
            meiso_wifi_runtime_host_private_storage.desired_publisher.bytes,
            sizeof(meiso_wifi_runtime_host_private_storage.desired_publisher.bytes),
            &resources[0], &state_config, &host->desired_publisher
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_state_replica_memory_required(&resources[1]);
    if (required == 0U || required > sizeof(meiso_wifi_runtime_host_private_storage.actual_replica.bytes) ||
        meiso_state_replica_init(
            meiso_wifi_runtime_host_private_storage.actual_replica.bytes,
            sizeof(meiso_wifi_runtime_host_private_storage.actual_replica.bytes),
            &resources[1], &host->actual_replica
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_stream_memory_required(&resources[2], &stream_config);
    if (required == 0U || required > sizeof(meiso_wifi_runtime_host_private_storage.stream.bytes) ||
        meiso_stream_init(
            meiso_wifi_runtime_host_private_storage.stream.bytes, sizeof(meiso_wifi_runtime_host_private_storage.stream.bytes),
            &resources[2], &stream_config, &host->stream
        ) != MEISO_STREAM_INIT_OK) {
        return false;
    }
    host->host_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST, host->session_config.local_incarnation
    };
    host->edge_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE, session_snapshot->remote_incarnation
    };
    return meiso_state_publisher_begin_delivery(
            host->desired_publisher, (MeisoDeliveryGeneration){1U}
        ) == MEISO_STATE_DELIVERY_STARTED &&
        meiso_state_replica_begin_owner(
            host->actual_replica, host->edge_owner
        ) == MEISO_STATE_OWNER_STARTED;
}
void meiso_wifi_runtime_host_private_emit_session_ready(const MeisoSessionSnapshot *snapshot)
{
    (void)printf(
        "{\"type\":\"session\",\"status\":\"READY\","
        "\"remote_identity_known\":true,\"identity_match\":true,"
        "\"session_id_known\":true,\"limits\":{"
        "\"max_frame_bytes\":%" PRIu32 ","
        "\"max_runtime_payload_bytes\":%" PRIu32 ","
        "\"max_registry_entries\":%" PRIu32 "}}\n",
        snapshot->negotiated_limits.max_frame_bytes,
        snapshot->negotiated_limits.max_runtime_payload_bytes,
        snapshot->negotiated_limits.max_registry_entries
    );
    (void)fflush(stdout);
}

static MeisoRuntimeRouteKey route_key(
    uint64_t generation,
    const MeisoRuntimeRouteDescriptor *route
)
{
    MeisoRuntimeRouteKey key;

    memset(&key, 0, sizeof(key));
    key.catalog_generation = generation;
    key.endpoint_id = route->endpoint_id;
    key.endpoint_incarnation = route->endpoint_incarnation;
    key.resource_id = route->resource.id;
    key.facet = route->facet;
    return key;
}

static bool validate_staged_catalog(
    MeisoWifiRuntimeHostPrivate *host,
    uint64_t generation
)
{
    static const MeisoRuntimeFacet expected_facets[
        MEISO_WIFI_TELEMETRY_RESOURCE_COUNT
    ] = {
        MEISO_RUNTIME_FACET_DESIRED,
        MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA
    };
    MeisoRuntimeSnapshot snapshot;
    MeisoRuntimeEndpointDescriptor endpoint;
    const MeisoEndpointId *expected_endpoint =
        meiso_wifi_telemetry_endpoint();
    const MeisoSchemaRef *expected_capability_schema =
        meiso_wifi_telemetry_capability_schema();
    const MeisoResourceDescriptor *expected_resources;
    bool found[MEISO_WIFI_TELEMETRY_RESOURCE_COUNT] = {false, false, false};
    size_t expected_count = 0U;
    size_t index;

    if (host == NULL || generation == 0U || expected_endpoint == NULL ||
        expected_capability_schema == NULL ||
        !meiso_runtime_snapshot(host->runtime, &snapshot) ||
        !snapshot.catalog_decision_pending || !snapshot.staged_catalog_present ||
        snapshot.staged_catalog_generation != generation ||
        snapshot.staged_endpoint_count != 1U ||
        snapshot.staged_route_count != MEISO_WIFI_TELEMETRY_RESOURCE_COUNT ||
        meiso_runtime_staged_endpoint_at(
            host->runtime, 0U, &endpoint
        ) != MEISO_RUNTIME_OK ||
        !meiso_endpoint_id_equal(endpoint.id, *expected_endpoint) ||
        !meiso_schema_ref_equal(
            endpoint.capability_schema, *expected_capability_schema
        ) ||
        meiso_wifi_telemetry_capability_decode(
            endpoint.capability, &host->capability
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK ||
        (!host->interactive &&
         !meiso_wifi_telemetry_parameters_supported(
             &host->capability, &host->bootstrap.desired
         ))) {
        return false;
    }
    expected_resources = meiso_wifi_telemetry_resources(&expected_count);
    if (expected_resources == NULL ||
        expected_count != MEISO_WIFI_TELEMETRY_RESOURCE_COUNT) {
        return false;
    }
    for (index = 0U; index < snapshot.staged_route_count; ++index) {
        MeisoRuntimeRouteDescriptor offered;
        size_t expected_index;
        bool matched = false;

        if (meiso_runtime_staged_route_at(
                host->runtime, index, &offered
            ) != MEISO_RUNTIME_OK ||
            !meiso_endpoint_id_equal(offered.endpoint_id, endpoint.id) ||
            offered.endpoint_incarnation != endpoint.incarnation) {
            return false;
        }
        for (expected_index = 0U;
             expected_index < expected_count;
             ++expected_index) {
            if (meiso_resource_id_equal(
                    offered.resource.id,
                    expected_resources[expected_index].id
                )) {
                if (found[expected_index] ||
                    offered.facet != expected_facets[expected_index] ||
                    !meiso_resource_descriptor_equal(
                        &offered.resource,
                        &expected_resources[expected_index]
                    )) {
                    return false;
                }
                found[expected_index] = true;
                matched = true;
                if (expected_index == 0U) {
                    host->desired_route = route_key(generation, &offered);
                } else if (expected_index == 1U) {
                    host->actual_route = route_key(generation, &offered);
                } else {
                    host->data_route = route_key(generation, &offered);
                }
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return found[0] && found[1] && found[2];
}

bool meiso_wifi_runtime_host_private_receive_and_accept_catalog(MeisoWifiRuntimeHostPrivate *host)
{
    struct timespec deadline;
    MeisoRuntimeEvent event;
    MeisoRuntimeSnapshot snapshot;

    if (!meiso_wifi_runtime_host_private_deadline_after_ms(&deadline, MEISO_WIFI_RUNTIME_HOST_PRIVATE_PHASE_TIMEOUT_MS) ||
        !meiso_wifi_runtime_host_private_wait_runtime_event(host, &deadline, &event) ||
        event.type != MEISO_RUNTIME_EVENT_CATALOG_OFFERED) {
        return false;
    }
    if (!validate_staged_catalog(host, event.catalog_generation)) {
        if (meiso_runtime_reject_catalog(
                host->runtime, MEISO_RUNTIME_CATALOG_REJECT_POLICY
            ) == MEISO_RUNTIME_OK) {
            (void)meiso_wifi_runtime_host_private_flush_outbound(host);
        }
        return false;
    }
    if (meiso_runtime_accept_catalog(host->runtime) != MEISO_RUNTIME_OK ||
        !meiso_wifi_runtime_host_private_flush_outbound(host) ||
        !meiso_runtime_snapshot(host->runtime, &snapshot) ||
        !snapshot.active_catalog_present ||
        snapshot.active_catalog_generation != event.catalog_generation) {
        return false;
    }
    host->catalog_active = true;
    (void)printf(
        "{\"type\":\"catalog\",\"status\":\"ACCEPTED\","
        "\"generation\":%" PRIu64 ",\"endpoint_count\":1,"
        "\"route_count\":3,\"capability\":{"
        "\"supported_fields\":%" PRIu32 ","
        "\"min_period_ms\":%" PRIu32 ","
        "\"max_period_ms\":%" PRIu32 ","
        "\"period_step_ms\":%" PRIu32 "}}\n",
        event.catalog_generation,
        host->capability.supported_fields,
        host->capability.min_period_ms,
        host->capability.max_period_ms,
        host->capability.period_step_ms
    );
    (void)fflush(stdout);
    return true;
}
