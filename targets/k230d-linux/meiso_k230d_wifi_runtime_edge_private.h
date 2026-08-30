#ifndef MEISO_TARGETS_K230D_LINUX_WIFI_RUNTIME_EDGE_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_WIFI_RUNTIME_EDGE_PRIVATE_H

/*
 * Build-only implementation seam for the K230D Wi-Fi Runtime Edge process.
 * This header owns its arena-backed storage, configuration, Runtime truth,
 * telemetry production state, and the narrow contracts shared by its source
 * modules. It is not installed and no object referenced here escapes main.
 */

#include <meiso/adapters/posix/tcp.h>
#include <meiso/runtime/runtime.h>
#include <meiso/core/session.h>
#include <meiso/core/state.h>
#include <meiso/edge/linux/wifi.h>
#include <meiso/profiles/wifi_telemetry.h>

#include <arpa/inet.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
#endif

enum {
    K230D_WIFI_RUNTIME_EDGE_MAX_FRAME_BYTES = 4096,
    K230D_WIFI_RUNTIME_EDGE_MAX_RUNTIME_PAYLOAD_BYTES = 2048,
    K230D_WIFI_RUNTIME_EDGE_MAX_REGISTRY_ENTRIES = 16,
    K230D_WIFI_RUNTIME_EDGE_IO_BYTES_PER_STEP = 1024,
    K230D_WIFI_RUNTIME_EDGE_SESSION_ARENA_BYTES = 8192,
    K230D_WIFI_RUNTIME_EDGE_TRANSPORT_ARENA_BYTES = 16384,
    K230D_WIFI_RUNTIME_EDGE_RUNTIME_ARENA_BYTES = 32768,
    K230D_WIFI_RUNTIME_EDGE_STATE_ARENA_BYTES = 4096,
    K230D_WIFI_RUNTIME_EDGE_CONFIG_LINE_BYTES = 256,
    K230D_WIFI_RUNTIME_EDGE_POLL_SLICE_MS = 100,
    K230D_WIFI_RUNTIME_EDGE_PHASE_TIMEOUT_MS = 10000,
    K230D_WIFI_RUNTIME_EDGE_CLOSE_TIMEOUT_MS = 2000,
    K230D_WIFI_RUNTIME_EDGE_CATALOG_GENERATION = 1,
    K230D_WIFI_RUNTIME_EDGE_STATE_DELIVERY_GENERATION = 1
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[K230D_WIFI_RUNTIME_EDGE_SESSION_ARENA_BYTES];
} K230dWifiRuntimeEdgeSessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[K230D_WIFI_RUNTIME_EDGE_TRANSPORT_ARENA_BYTES];
} K230dWifiRuntimeEdgeTransportArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[K230D_WIFI_RUNTIME_EDGE_RUNTIME_ARENA_BYTES];
} K230dWifiRuntimeEdgeRuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[K230D_WIFI_RUNTIME_EDGE_STATE_ARENA_BYTES];
} K230dWifiRuntimeEdgeStateArena;

typedef struct {
    K230dWifiRuntimeEdgeSessionArena session;
    K230dWifiRuntimeEdgeTransportArena transport;
    K230dWifiRuntimeEdgeRuntimeArena runtime;
    K230dWifiRuntimeEdgeStateArena desired_replica;
    K230dWifiRuntimeEdgeStateArena actual_publisher;
} K230dWifiRuntimeEdgeStorage;

typedef struct {
    char listen_ipv4[INET_ADDRSTRLEN];
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    MeisoPeerId local_edge_peer_id;
    MeisoPeerId expected_host_peer_id;
    uint16_t listen_port;
    MeisoWifiTelemetryFieldMask allowed_fields;
    uint32_t min_period_ms;
    uint32_t max_period_ms;
    uint32_t period_step_ms;
} K230dWifiRuntimeEdgeConfig;

typedef struct {
    bool present;
    MeisoRuntimeRouteKey route;
    MeisoDeliveryGeneration generation;
    MeisoOwnerScope owner;
    uint64_t version;
} K230dWifiRuntimeEdgePendingStateAccept;

typedef enum {
    K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_NONE = 0,
    K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_STOPPED,
    K230D_WIFI_RUNTIME_EDGE_ACTUAL_FOLLOW_UP_RUNNING
} K230dWifiRuntimeEdgeActualFollowUp;

typedef struct {
    bool present;
    bool published;
    MeisoRuntimeDesiredReference observed;
    MeisoRuntimeDesiredReference applied;
    MeisoRuntimeApplyResult apply_result;
    MeisoRuntimeAvailability availability;
    bool effective_enabled;
    MeisoRuntimeProductionStatus production_status;
    MeisoRuntimeProductionReason reason;
    uint64_t production_epoch;
    MeisoWifiTelemetryEffectiveParameters effective;
    K230dWifiRuntimeEdgeActualFollowUp follow_up;
    MeisoWifiTelemetryDesiredParameters follow_up_parameters;
} K230dWifiRuntimeEdgeActualTransition;

typedef struct {
    bool present;
    uint64_t production_epoch;
    uint8_t payload[MEISO_WIFI_TELEMETRY_SAMPLE_WIRE_SIZE];
    MeisoStreamSample sample;
} K230dWifiRuntimeEdgePendingSample;

typedef struct {
    MeisoWifiTelemetryFieldMask seen_fields;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
    uint64_t rx_errors;
    uint64_t tx_errors;
} K230dWifiRuntimeEdgeCounterContinuity;

typedef struct {
    K230dWifiRuntimeEdgeConfig private_config;
    MeisoWifiLinuxAdapter wifi;
    MeisoWifiTelemetryCapability capability;
    uint8_t capability_payload[MEISO_WIFI_TELEMETRY_CAPABILITY_WIRE_SIZE];
    size_t capability_payload_size;

    int listener_fd;
    int connection_fd;
    MeisoSessionConfig session_config;
    MeisoSession *session;
    MeisoPosixTcp *transport;
    MeisoRuntime *runtime;
    MeisoStateReplica *desired_replica;
    MeisoStatePublisher *actual_publisher;
    MeisoOwnerScope edge_owner;
    MeisoOwnerScope host_owner;

    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeRouteDescriptor routes[MEISO_WIFI_TELEMETRY_RESOURCE_COUNT];
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeRouteKey data_route;

    K230dWifiRuntimeEdgePendingStateAccept pending_accept;
    K230dWifiRuntimeEdgeActualTransition actual_outbound;
    K230dWifiRuntimeEdgeActualTransition actual_in_flight_value;
    K230dWifiRuntimeEdgeActualTransition actual_next;
    K230dWifiRuntimeEdgePendingSample pending_sample;
    MeisoWifiTelemetryDesiredParameters current_parameters;
    MeisoRuntimeDesiredReference current_desired;
    MeisoRuntimeDesiredReference production_applied_desired;
    MeisoWifiTelemetryEffectiveParameters production_effective;
    K230dWifiRuntimeEdgeCounterContinuity counter_continuity;

    uint64_t actual_version;
    uint64_t production_epoch_high_water;
    uint64_t active_production_epoch;
    uint64_t stream_sequence;
    uint64_t next_sample_ns;
    uint64_t next_probe_ns;
    uint64_t phase_deadline_ns;
    uint64_t samples_queued;
    uint64_t clock_epoch;

    bool runtime_initialized;
    bool catalog_offered;
    bool catalog_active;
    bool actual_in_flight;
    bool desired_profile_valid;
    bool desired_enabled;
    bool production_active;
    bool production_ever_started;
    bool source_continuity_lost;
    bool peer_closed_normally;
} K230dWifiRuntimeEdge;

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
extern volatile sig_atomic_t
    meiso_k230d_wifi_runtime_edge_private_g_stop_requested;

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
void meiso_k230d_wifi_runtime_edge_private_close_fd(int *fd);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_monotonic_now_ns(
    uint64_t *out_value
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
uint64_t meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
    uint64_t now_ns,
    uint32_t milliseconds
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_parse_config(
    const char *path,
    K230dWifiRuntimeEdgeConfig *out_config
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_initialize_profile(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_initialize_resources(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_initialize_session_and_transport(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_initialize_runtime(
    K230dWifiRuntimeEdge *edge,
    uint64_t now_ns
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_session_tx_idle(
    const K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_try_offer_catalog(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
void meiso_k230d_wifi_runtime_edge_private_mark_source_continuity_lost(
    K230dWifiRuntimeEdge *edge,
    const char *cause
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_handle_runtime_message(
    K230dWifiRuntimeEdge *edge,
    const MeisoSessionRuntimeMessage *message,
    uint64_t now_ns
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_try_send_pending_sample(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_try_send_state_accept(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_publish_actual_if_needed(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_request_running_for_current_desired(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_counter_continuity_self_test(
    void
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_drive_application(
    K230dWifiRuntimeEdge *edge,
    uint64_t now_ns
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
bool meiso_k230d_wifi_runtime_edge_private_run_connection(
    K230dWifiRuntimeEdge *edge
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
void meiso_k230d_wifi_runtime_edge_private_flush_local_close(
    K230dWifiRuntimeEdge *edge,
    MeisoSessionCloseReason reason
);

MEISO_K230D_WIFI_RUNTIME_EDGE_PRIVATE_API
int meiso_k230d_wifi_runtime_edge_private_run(int argc, char **argv);

#endif
