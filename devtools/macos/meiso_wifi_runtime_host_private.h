#ifndef MEISO_WIFI_RUNTIME_HOST_PRIVATE_H
#define MEISO_WIFI_RUNTIME_HOST_PRIVATE_H

#include <meiso/adapters/posix/tcp.h>
#include <meiso/profiles/wifi_telemetry.h>
#include <meiso/runtime/runtime.h>

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#if defined(__clang__) || defined(__GNUC__)
#define MEISO_WIFI_RUNTIME_HOST_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_WIFI_RUNTIME_HOST_PRIVATE_API
#endif

enum {
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_FRAME_BYTES = 4096,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_RUNTIME_BYTES = 2048,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_REGISTRY_ENTRIES = 16,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_IO_STEP_BYTES = 1024,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONNECT_TIMEOUT_MS = 10000,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_PHASE_TIMEOUT_MS = 20000,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_SAMPLES = 64,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_STOPPING_SAMPLES = 64,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_STREAM_QUEUE_SLOTS = 8,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_STREAM_HISTORY_SLOTS = 8,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_MAX_STREAM_DEPENDENCY_BYTES = 64,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_SESSION_ARENA_BYTES = 8192,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_ADAPTER_ARENA_BYTES = 16384,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_RUNTIME_ARENA_BYTES = 32768,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_RESOURCE_ARENA_BYTES = 16384,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONFIG_LINE_BYTES = 256,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_LINE_BYTES = 256
};

typedef union {
    max_align_t alignment;
    uint8_t bytes[MEISO_WIFI_RUNTIME_HOST_PRIVATE_SESSION_ARENA_BYTES];
} MeisoWifiRuntimeHostPrivateSessionArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[MEISO_WIFI_RUNTIME_HOST_PRIVATE_ADAPTER_ARENA_BYTES];
} MeisoWifiRuntimeHostPrivateAdapterArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[MEISO_WIFI_RUNTIME_HOST_PRIVATE_RUNTIME_ARENA_BYTES];
} MeisoWifiRuntimeHostPrivateRuntimeArena;

typedef union {
    max_align_t alignment;
    uint8_t bytes[MEISO_WIFI_RUNTIME_HOST_PRIVATE_RESOURCE_ARENA_BYTES];
} MeisoWifiRuntimeHostPrivateResourceArena;

typedef struct {
    struct in_addr connect_address;
    uint16_t connect_port;
    MeisoPeerId local_peer_id;
    MeisoPeerId expected_edge_peer_id;
    MeisoWifiTelemetryDesiredParameters desired;
    uint32_t sample_count;
} MeisoWifiRuntimeHostPrivateBootstrapConfig;

typedef struct {
    MeisoWifiRuntimeHostPrivateSessionArena session;
    MeisoWifiRuntimeHostPrivateAdapterArena adapter;
    MeisoWifiRuntimeHostPrivateRuntimeArena runtime;
    MeisoWifiRuntimeHostPrivateResourceArena desired_publisher;
    MeisoWifiRuntimeHostPrivateResourceArena actual_replica;
    MeisoWifiRuntimeHostPrivateResourceArena stream;
} MeisoWifiRuntimeHostPrivateStorage;

/*
 * Build-only process state. The process owns fd and closes it exactly once;
 * Session, adapter, Runtime, State and Stream handles borrow the static arenas
 * below. Runtime views remain short-lived borrows governed by their SDK APIs.
 * Field order is intentionally identical to the former single-TU program.
 */
typedef struct {
    int fd;
    MeisoWifiRuntimeHostPrivateBootstrapConfig bootstrap;
    MeisoSessionConfig session_config;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoRuntime *runtime;
    MeisoStatePublisher *desired_publisher;
    MeisoStateReplica *actual_replica;
    MeisoStream *stream;
    MeisoOwnerScope host_owner;
    MeisoOwnerScope edge_owner;
    MeisoRuntimeRouteKey desired_route;
    MeisoRuntimeRouteKey actual_route;
    MeisoRuntimeRouteKey data_route;
    MeisoWifiTelemetryCapability capability;
    MeisoWifiTelemetryEffectiveParameters active_effective;
    MeisoOwnerScope stream_producer;
    uint64_t stream_epoch;
    uint64_t running_production_epoch;
    uint64_t samples_received;
    bool catalog_active;
    bool active_effective_present;
    bool stream_producer_active;
    bool production_active;
    bool interactive;
} MeisoWifiRuntimeHostPrivate;

typedef enum {
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_INVALID = 0,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_APPLY,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_CLOSE
} MeisoWifiRuntimeHostPrivateControlType;

typedef struct {
    MeisoWifiRuntimeHostPrivateControlType type;
    bool enabled;
    MeisoWifiTelemetryDesiredParameters parameters;
} MeisoWifiRuntimeHostPrivateControlCommand;

typedef struct {
    char bytes[MEISO_WIFI_RUNTIME_HOST_PRIVATE_CONTROL_LINE_BYTES];
    size_t size;
} MeisoWifiRuntimeHostPrivateControlInput;

typedef enum {
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED = 0,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_COMMAND,
    MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_RUNTIME
} MeisoWifiRuntimeHostPrivateInteractiveItemType;

MEISO_WIFI_RUNTIME_HOST_PRIVATE_API extern
MeisoWifiRuntimeHostPrivateStorage meiso_wifi_runtime_host_private_storage;

MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_load_config(
    const char *path,
    bool interactive,
    MeisoWifiRuntimeHostPrivateBootstrapConfig *out_config
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API void
meiso_wifi_runtime_host_private_close_fd(int *fd);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_deadline_after_ms(
    struct timespec *out_deadline,
    uint64_t ms
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API int
meiso_wifi_runtime_host_private_deadline_wait_ms(
    const struct timespec *deadline
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API int
meiso_wifi_runtime_host_private_connect_numeric_ipv4(
    const MeisoWifiRuntimeHostPrivateBootstrapConfig *config
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_initialize_session_transport(
    MeisoWifiRuntimeHostPrivate *host
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_flush_outbound(
    MeisoWifiRuntimeHostPrivate *host
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_wait_session_ready(
    MeisoWifiRuntimeHostPrivate *host,
    MeisoSessionSnapshot *out_snapshot
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_wait_runtime_event(
    MeisoWifiRuntimeHostPrivate *host,
    const struct timespec *deadline,
    MeisoRuntimeEvent *out_event
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_initialize_runtime_resources(
    MeisoWifiRuntimeHostPrivate *host,
    const MeisoSessionSnapshot *session_snapshot
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API void
meiso_wifi_runtime_host_private_emit_session_ready(
    const MeisoSessionSnapshot *snapshot
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_receive_and_accept_catalog(
    MeisoWifiRuntimeHostPrivate *host
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_publish_desired(
    MeisoWifiRuntimeHostPrivate *host,
    uint64_t version,
    bool enabled
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_wait_desired_and_actual(
    MeisoWifiRuntimeHostPrivate *host,
    uint64_t desired_version,
    MeisoRuntimeProductionStatus target_status
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_receive_requested_samples(
    MeisoWifiRuntimeHostPrivate *host
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_handle_interactive_runtime_event(
    MeisoWifiRuntimeHostPrivate *host,
    const MeisoRuntimeEvent *event,
    uint64_t desired_version,
    bool desired_enabled
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_extract_control_command(
    MeisoWifiRuntimeHostPrivateControlInput *input,
    MeisoWifiRuntimeHostPrivateControlCommand *out_command,
    bool *out_present
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API
MeisoWifiRuntimeHostPrivateInteractiveItemType
meiso_wifi_runtime_host_private_wait_interactive_item(
    MeisoWifiRuntimeHostPrivate *host,
    MeisoWifiRuntimeHostPrivateControlInput *input,
    MeisoWifiRuntimeHostPrivateControlCommand *out_command,
    MeisoRuntimeEvent *out_event
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_run_interactive(
    MeisoWifiRuntimeHostPrivate *host
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API bool
meiso_wifi_runtime_host_private_normal_close(
    MeisoWifiRuntimeHostPrivate *host
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API void
meiso_wifi_runtime_host_private_best_effort_close(
    MeisoWifiRuntimeHostPrivate *host
);
MEISO_WIFI_RUNTIME_HOST_PRIVATE_API void
meiso_wifi_runtime_host_private_emit_failure(const char *stage);

#endif
