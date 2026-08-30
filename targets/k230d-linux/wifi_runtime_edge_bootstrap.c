#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_wifi_runtime_edge_private.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Owns process-lifetime SDK arenas and constructs Profile, State, Session,
 * transport, and Runtime objects in dependency order. The arena storage never
 * escapes this target and remains valid until process exit.
 */

static K230dWifiRuntimeEdgeStorage g_storage;

static bool read_entropy(void *output, size_t size)
{
    uint8_t *bytes = output;
    size_t offset = 0U;
    int fd;

    if (output == NULL || size == 0U) {
        return false;
    }
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    while (offset < size) {
        const ssize_t count = read(fd, bytes + offset, size - offset);
        if (count > 0) {
            offset += (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            meiso_k230d_wifi_runtime_edge_private_close_fd(&fd);
            return false;
        }
    }
    meiso_k230d_wifi_runtime_edge_private_close_fd(&fd);
    return true;
}

static bool random_nonzero_u64(uint64_t *out_value)
{
    uint64_t value;

    if (out_value == NULL || !read_entropy(&value, sizeof(value))) {
        return false;
    }
    value &= UINT64_C(0x7fffffffffffffff);
    *out_value = value == 0U ? 1U : value;
    return true;
}

static bool random_nonce(MeisoSessionNonce *out_nonce)
{
    if (out_nonce == NULL || !read_entropy(out_nonce, sizeof(*out_nonce))) {
        return false;
    }
    if (!meiso_session_nonce_valid(*out_nonce)) {
        out_nonce->low = 1U;
    }
    return true;
}

static MeisoRuntimeRouteKey route_key(
    const K230dWifiRuntimeEdge *edge,
    const MeisoResourceDescriptor *resource,
    MeisoRuntimeFacet facet
)
{
    const MeisoRuntimeRouteKey key = {
        K230D_WIFI_RUNTIME_EDGE_CATALOG_GENERATION,
        *meiso_wifi_telemetry_endpoint(),
        edge->session_config.local_incarnation,
        resource->id,
        facet
    };
    return key;
}

bool meiso_k230d_wifi_runtime_edge_private_initialize_profile(K230dWifiRuntimeEdge *edge)
{
    MeisoWifiLinuxAdapterConfig adapter_config;
    MeisoWifiTelemetryFieldMask probed_fields = 0U;
    size_t encoded_size = 0U;

    memset(&adapter_config, 0, sizeof(adapter_config));
    memmove(
        adapter_config.interface_name,
        edge->private_config.interface_name,
        strlen(edge->private_config.interface_name) + 1U
    );
    adapter_config.allowed_fields = edge->private_config.allowed_fields;
    if (meiso_wifi_linux_adapter_init(&edge->wifi, &adapter_config) !=
            MEISO_WIFI_LINUX_ADAPTER_OK ||
        meiso_wifi_linux_adapter_probe(&edge->wifi, &probed_fields) !=
            MEISO_WIFI_LINUX_ADAPTER_OK) {
        return false;
    }
    edge->capability.supported_fields =
        edge->private_config.allowed_fields & probed_fields;
    edge->capability.min_period_ms = edge->private_config.min_period_ms;
    edge->capability.max_period_ms = edge->private_config.max_period_ms;
    edge->capability.period_step_ms = edge->private_config.period_step_ms;
    if (edge->capability.supported_fields == 0U ||
        meiso_wifi_telemetry_capability_encode(
            &edge->capability,
            (MeisoMutableBuffer){
                edge->capability_payload,
                sizeof(edge->capability_payload)
            },
            &encoded_size
        ) != MEISO_WIFI_TELEMETRY_CODEC_OK) {
        return false;
    }
    edge->capability_payload_size = encoded_size;
    return true;
}

bool meiso_k230d_wifi_runtime_edge_private_initialize_resources(K230dWifiRuntimeEdge *edge)
{
    const MeisoResourceDescriptor *desired =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_DESIRED
        );
    const MeisoResourceDescriptor *actual =
        meiso_wifi_telemetry_resource_for_facet(
            MEISO_RUNTIME_FACET_ACTUAL
        );
    const MeisoStateConfig state_config = {
        MEISO_STATE_DELIVERY_CONVERGENT
    };
    size_t required;

    if (desired == NULL || actual == NULL) {
        return false;
    }
    required = meiso_state_replica_memory_required(desired);
    if (required == 0U || required > sizeof(g_storage.desired_replica.bytes) ||
        meiso_state_replica_init(
            g_storage.desired_replica.bytes,
            required,
            desired,
            &edge->desired_replica
        ) != MEISO_STATE_INIT_OK) {
        return false;
    }
    required = meiso_state_publisher_memory_required(actual, &state_config);
    if (required == 0U ||
        required > sizeof(g_storage.actual_publisher.bytes) ||
        meiso_state_publisher_init(
            g_storage.actual_publisher.bytes,
            required,
            actual,
            &state_config,
            &edge->actual_publisher
        ) != MEISO_STATE_INIT_OK ||
        meiso_state_publisher_begin_delivery(
            edge->actual_publisher,
            (MeisoDeliveryGeneration){K230D_WIFI_RUNTIME_EDGE_STATE_DELIVERY_GENERATION}
        ) != MEISO_STATE_DELIVERY_STARTED) {
        return false;
    }
    return true;
}

bool
meiso_k230d_wifi_runtime_edge_private_initialize_session_and_transport(
    K230dWifiRuntimeEdge *edge
)
{
    MeisoPosixTcpConfig transport_config;
    size_t required;

    memset(&edge->session_config, 0, sizeof(edge->session_config));
    edge->session_config.local_role = MEISO_SESSION_ROLE_EDGE;
    edge->session_config.local_peer_id =
        edge->private_config.local_edge_peer_id;
    edge->session_config.expected_peer_id =
        edge->private_config.expected_host_peer_id;
    edge->session_config.protocol = (MeisoProtocolVersion){1U, 0U};
    edge->session_config.profile = *meiso_wifi_telemetry_profile();
    edge->session_config.limits = (MeisoSessionLimits){
        K230D_WIFI_RUNTIME_EDGE_MAX_FRAME_BYTES,
        K230D_WIFI_RUNTIME_EDGE_MAX_RUNTIME_PAYLOAD_BYTES,
        K230D_WIFI_RUNTIME_EDGE_MAX_REGISTRY_ENTRIES
    };
    if (!random_nonzero_u64(&edge->session_config.local_incarnation) ||
        !random_nonce(&edge->session_config.local_nonce)) {
        return false;
    }
    edge->edge_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_EDGE,
        edge->session_config.local_incarnation
    };
    edge->clock_epoch = edge->session_config.local_incarnation;
    required = meiso_session_memory_required(&edge->session_config);
    if (required == 0U || required > sizeof(g_storage.session.bytes) ||
        meiso_session_init(
            g_storage.session.bytes,
            required,
            &edge->session_config,
            &edge->session
        ) != MEISO_SESSION_INIT_OK) {
        return false;
    }
    transport_config = (MeisoPosixTcpConfig){
        edge->connection_fd,
        edge->session,
        K230D_WIFI_RUNTIME_EDGE_MAX_FRAME_BYTES,
        K230D_WIFI_RUNTIME_EDGE_IO_BYTES_PER_STEP
    };
    required = meiso_posix_tcp_memory_required(&transport_config);
    if (required == 0U || required > sizeof(g_storage.transport.bytes) ||
        meiso_posix_tcp_init(
            g_storage.transport.bytes,
            required,
            &transport_config,
            &edge->transport
        ) != MEISO_POSIX_TCP_INIT_OK ||
        meiso_session_start(edge->session) != MEISO_SESSION_START_STARTED) {
        return false;
    }
    return true;
}

bool meiso_k230d_wifi_runtime_edge_private_initialize_runtime(
    K230dWifiRuntimeEdge *edge,
    uint64_t now_ns
)
{
    MeisoSessionSnapshot session_snapshot;
    MeisoRuntimeConfig runtime_config;
    const MeisoResourceDescriptor *desired;
    const MeisoResourceDescriptor *actual;
    const MeisoResourceDescriptor *data;
    size_t required;

    if (!meiso_session_snapshot(edge->session, &session_snapshot) ||
        session_snapshot.state != MEISO_SESSION_STATE_READY ||
        !session_snapshot.remote_identity_known ||
        session_snapshot.remote_incarnation == 0U) {
        return false;
    }
    edge->host_owner = (MeisoOwnerScope){
        MEISO_OWNER_ROLE_HOST,
        session_snapshot.remote_incarnation
    };
    if (meiso_state_replica_begin_owner(
            edge->desired_replica, edge->host_owner
        ) != MEISO_STATE_OWNER_STARTED) {
        return false;
    }
    memset(&runtime_config, 0, sizeof(runtime_config));
    runtime_config.session = edge->session;
    runtime_config.max_endpoints = 1U;
    runtime_config.max_endpoint_history = 2U;
    runtime_config.max_routes = MEISO_WIFI_TELEMETRY_RESOURCE_COUNT;
    runtime_config.max_capability_bytes =
        MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE;
    runtime_config.max_desired_profile_bytes_per_endpoint =
        MEISO_WIFI_TELEMETRY_DESIRED_PARAMETERS_WIRE_SIZE;
    runtime_config.max_actual_profile_bytes_per_endpoint =
        MEISO_WIFI_TELEMETRY_EFFECTIVE_PARAMETERS_WIRE_SIZE;
    required = meiso_runtime_memory_required(&runtime_config);
    if (required == 0U || required > sizeof(g_storage.runtime.bytes) ||
        meiso_runtime_init(
            g_storage.runtime.bytes,
            required,
            &runtime_config,
            &edge->runtime
        ) != MEISO_RUNTIME_INIT_OK) {
        return false;
    }

    desired = meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_DESIRED
    );
    actual = meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_ACTUAL
    );
    data = meiso_wifi_telemetry_resource_for_facet(
        MEISO_RUNTIME_FACET_DATA
    );
    if (desired == NULL || actual == NULL || data == NULL) {
        return false;
    }
    edge->endpoint = (MeisoRuntimeEndpointDescriptor){
        *meiso_wifi_telemetry_endpoint(),
        edge->session_config.local_incarnation,
        *meiso_wifi_telemetry_capability_schema(),
        {
            edge->capability_payload,
            edge->capability_payload_size
        }
    };
    edge->routes[0] = (MeisoRuntimeRouteDescriptor){
        edge->endpoint.id,
        edge->endpoint.incarnation,
        *desired,
        MEISO_RUNTIME_FACET_DESIRED
    };
    edge->routes[1] = (MeisoRuntimeRouteDescriptor){
        edge->endpoint.id,
        edge->endpoint.incarnation,
        *actual,
        MEISO_RUNTIME_FACET_ACTUAL
    };
    edge->routes[2] = (MeisoRuntimeRouteDescriptor){
        edge->endpoint.id,
        edge->endpoint.incarnation,
        *data,
        MEISO_RUNTIME_FACET_DATA
    };
    edge->desired_route = route_key(
        edge, desired, MEISO_RUNTIME_FACET_DESIRED
    );
    edge->actual_route = route_key(
        edge, actual, MEISO_RUNTIME_FACET_ACTUAL
    );
    edge->data_route = route_key(edge, data, MEISO_RUNTIME_FACET_DATA);
    edge->runtime_initialized = true;
    edge->phase_deadline_ns = meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
        now_ns, K230D_WIFI_RUNTIME_EDGE_PHASE_TIMEOUT_MS
    );
    (void)printf(
        "{\"type\":\"session\",\"phase\":\"handshake\","
        "\"status\":\"READY\"}\n"
    );
    return true;
}
