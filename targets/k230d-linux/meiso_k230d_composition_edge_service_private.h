#ifndef MEISO_TARGETS_K230D_LINUX_COMPOSITION_EDGE_SERVICE_PRIVATE_H
#define MEISO_TARGETS_K230D_LINUX_COMPOSITION_EDGE_SERVICE_PRIVATE_H

/*
 * Build-only implementation seam for the foreground K230D Edge service.
 * This header owns process-local configuration, descriptor/arena ownership,
 * the bounded thermal worker, and the shutdown ledger. It is never installed.
 * The process is single-threaded except for the thermal worker, which touches
 * only its request/result slot and publishes completion through atomic_bool.
 */

#include <meiso/edge/linux/composition_runtime.h>

#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API \
    __attribute__((visibility("hidden")))
#else
#define MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
#endif

enum {
    CONFIG_LINE_BYTES = 384,
    CONFIG_MAX_ROWS = 64,
    POLL_SLICE_MS = 10,
    CONTROL_IO_BYTES_PER_STEP = 4096,
    CONTROL_ADAPTER_RESERVE_BYTES = 65536,
    CONTROL_RUNTIME_RESERVE_BYTES = 400000,
    CONTROL_SCHEDULER_RESERVE_BYTES = 65536,
    MEDIA_IO_BYTES_PER_STEP = 4096,
    THERMAL_WORKER_BUDGET_MS = 100,
    ENDPOINT_COUNT = 6
};

typedef struct {
    char listen_ipv4[INET_ADDRSTRLEN];
    char interface_name[MEISO_WIFI_LINUX_INTERFACE_NAME_CAPACITY];
    uint16_t control_port;
    uint16_t media_port;
    MeisoPeerId local_edge_peer_id;
    MeisoPeerId expected_host_peer_id;
    uint64_t local_incarnation;
    uint64_t catalog_generation;
    uint64_t endpoint_incarnation_base;
} K230dCompositionEdgeServiceConfig;

typedef struct {
    pthread_t thread;
    atomic_bool done;
    bool active;
    MeisoK230dCompositionEdgeRuntimeThermalRequest request;
    MeisoK230dCompositionEdgeRuntimeThermalResult result;
    MeisoK230dCompositionEdgeRuntimeThermalReadStatus status;
} K230dCompositionEdgeThermalWorker;

typedef struct {
    K230dCompositionEdgeServiceConfig private_config;
    MeisoK230dCompositionEdgeRuntime *edge_runtime;
    void *edge_runtime_arena;
    size_t edge_runtime_arena_bytes;
    int control_listener;
    int media_listener;
    int control_fd;
    int media_fd;
    bool media_bound;
    uint64_t edge_clock_epoch;
    K230dCompositionEdgeThermalWorker thermal;
} K230dCompositionEdgeService;

typedef struct {
    bool runtime_initialized;
    MeisoK230dCompositionEdgeRuntimeStatus abort_status;
    bool snapshot_present;
    MeisoK230dCompositionEdgeRuntimeSnapshot snapshot;
    bool thermal_joined;
} K230dCompositionEdgeShutdownLedger;

typedef uint8_t PendingMediaCandidateStatus;
enum {
    PENDING_MEDIA_CANDIDATE_LIVE = 1,
    PENDING_MEDIA_CANDIDATE_CLOSED = 2
};

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
extern volatile sig_atomic_t
    meiso_k230d_composition_edge_service_private_g_stop_requested;

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_install_signal_handlers(
    void
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
void meiso_k230d_composition_edge_service_private_close_fd(int *fd);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_set_nonblocking(int fd);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
PendingMediaCandidateStatus
meiso_k230d_composition_edge_service_private_pending_media_candidate_status(
    int fd,
    short revents
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_monotonic_now_ns(
    uint64_t *out_value
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_copy_text(
    char *output,
    size_t capacity,
    const char *input
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_parse_private_config(
    const char *path,
    K230dCompositionEdgeServiceConfig *out_config
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
int meiso_k230d_composition_edge_service_private_make_listener(
    const char *ipv4,
    uint16_t port,
    bool nonblocking
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
int meiso_k230d_composition_edge_service_private_accept_connected(
    int listener,
    bool wait_for_one
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_start_thermal_worker(
    K230dCompositionEdgeService *service
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_collect_thermal_worker(
    K230dCompositionEdgeService *service,
    uint64_t now_ns
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_join_thermal_worker(
    K230dCompositionEdgeService *service
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
const char *meiso_k230d_composition_edge_service_private_terminal_reason_name(
    MeisoK230dCompositionEdgeRuntimeTerminalReason reason
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_final_shutdown_clean(
    int stop_signal,
    bool loop_stopped,
    const K230dCompositionEdgeShutdownLedger *ledger
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_initialize_edge_runtime(
    K230dCompositionEdgeService *service
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
bool meiso_k230d_composition_edge_service_private_run_loop(
    K230dCompositionEdgeService *service
);

MEISO_K230D_COMPOSITION_EDGE_SERVICE_PRIVATE_API
int meiso_k230d_composition_edge_service_private_run(
    int argc,
    char **argv
);

#endif
