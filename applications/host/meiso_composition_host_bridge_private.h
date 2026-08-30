#ifndef MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_H
#define MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <meiso/host/remote_edge.h>
#include <meiso/simulation/composition_edge.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

enum {
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LINE_BYTES = 512,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_LINE_BYTES = 16384,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONNECT_TIMEOUT_MS = 10000,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_IO_STEP_BYTES = 4096,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_ADAPTER_BYTES = 65536,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_RUNTIME_BYTES = 400000,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_SCHEDULER_BYTES = 65536,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_MEDIA_CONNECTION_SLOTS = 4,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS =
        MEISO_COMPOSITION_SCENE_MAX_OBJECTS,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CUBE_BYTES = 512,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CMFB_HEADER_BYTES = 80,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_FRAME_BYTES =
        MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES >
                MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES ?
            MEISO_COMPOSITION_RENDER_MEDIA_MAX_ACCESS_UNIT_BYTES :
            MEISO_COMPOSITION_CAMERA_MEDIA_MAX_ACCESS_UNIT_BYTES,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_FRAME_RECORD_BYTES =
        MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CMFB_HEADER_BYTES +
        MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_FRAME_BYTES,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_POLL_TIMEOUT_MS = 1000
};

enum {
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CONTROL_ADDRESS =
        UINT64_C(1) << 0U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CONTROL_PORT =
        UINT64_C(1) << 1U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_MEDIA_ADDRESS =
        UINT64_C(1) << 2U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_MEDIA_PORT =
        UINT64_C(1) << 3U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LOCAL_PEER_HIGH =
        UINT64_C(1) << 4U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LOCAL_PEER_LOW =
        UINT64_C(1) << 5U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_EDGE_PEER_HIGH =
        UINT64_C(1) << 6U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_EDGE_PEER_LOW =
        UINT64_C(1) << 7U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_LOCAL_INCARNATION =
        UINT64_C(1) << 8U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_NONCE_HIGH =
        UINT64_C(1) << 9U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_NONCE_LOW =
        UINT64_C(1) << 10U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_TARGET_MANIFEST =
        UINT64_C(1) << 11U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_WIFI_FIELDS =
        UINT64_C(1) << 12U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_DEVICE_FIELDS =
        UINT64_C(1) << 13U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CATALOG_GENERATION =
        UINT64_C(1) << 14U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_WIFI_INCARNATION =
        UINT64_C(1) << 15U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_PRESENTATION_INCARNATION =
        UINT64_C(1) << 16U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_SCENE_INCARNATION =
        UINT64_C(1) << 17U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_DEVICE_INCARNATION =
        UINT64_C(1) << 18U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_RENDER_INCARNATION =
        UINT64_C(1) << 19U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_CAMERA_INCARNATION =
        UINT64_C(1) << 20U,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONFIG_REQUIRED =
        (UINT64_C(1) << 21U) - 1U
};

typedef struct {
    struct sockaddr_storage control_address;
    socklen_t control_address_size;
    uint16_t control_port;
    struct sockaddr_storage media_address;
    socklen_t media_address_size;
    uint16_t media_port;
    MeisoPeerId local_peer_id;
    MeisoPeerId edge_peer_id;
    uint64_t local_incarnation;
    MeisoSessionNonce nonce;
    uint64_t catalog_generation;
    MeisoCompositionProfileCatalogConfig catalog;
} MeisoCompositionHostBridgePrivateConfig;

typedef struct {
    bool used;
    uint64_t editor_id;
    MeisoCompositionSceneObjectId object_id;
} MeisoCompositionHostBridgePrivateEditorObjectIdentity;

typedef struct {
    uint8_t bytes[
        MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_FRAME_RECORD_BYTES
    ];
    size_t size;
    size_t offset;
    bool pending;
} MeisoCompositionHostBridgePrivateFrameOutput;

typedef uint8_t MeisoCompositionHostBridgePrivateBackend;
enum {
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_REMOTE = 1,
    MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_SIMULATOR = 2
};

/*
 * The process owns control_fd, media_fd, and both arenas. Installed Runtime
 * adapters borrow control_fd/media_fd until abort or unbind; frame_fd remains
 * a caller-owned borrow and is never closed here. A descriptor not installed
 * is -1, and a backend-specific handle not installed in its arena is NULL.
 */
typedef struct {
    MeisoCompositionHostBridgePrivateBackend backend;
    MeisoCompositionHostBridgePrivateConfig config;
    int control_fd;
    int media_fd;
    bool media_connecting;
    MeisoCompositionHostRuntimeMediaRouteMask media_connect_mask;
    struct timespec media_connect_deadline;
    struct timespec media_retry_after;
    int frame_fd;
    void *host_runtime_arena;
    size_t host_runtime_arena_size;
    MeisoCompositionHostRuntime *host_runtime;
    void *edge_arena;
    size_t edge_arena_size;
    MeisoEdgeHandle *edge;
    char input[MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONTROL_LINE_BYTES];
    size_t input_size;
    /* Set only after read(STDIN_FILENO) returns zero, never from POLLHUP. */
    bool stdin_eof;
    uint8_t cube_bytes[MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CUBE_BYTES];
    size_t cube_size;
    bool cube_loaded;
    MeisoContentId cube_content_id;
    MeisoCompositionHostBridgePrivateEditorObjectIdentity objects[
        MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_EDITOR_OBJECTS
    ];
    uint64_t next_object_serial;
    uint64_t scene_space_epoch;
    uint64_t host_clock_epoch;
    uint64_t simulator_monotonic_ns;
    MeisoCompositionHostBridgePrivateFrameOutput frame_output;
    uint8_t frame_drain_cursor;
    uint8_t loop_cursor;
    bool close_requested;
} MeisoCompositionHostBridgePrivateProcess;

void meiso_composition_host_bridge_private_close_fd(int *fd);
bool meiso_composition_host_bridge_private_parse_u64_text(
    const char *text,
    uint64_t maximum,
    uint64_t *out_value
);
MeisoCompositionEdgeSimulatorConfig
meiso_composition_host_bridge_private_offline_simulator_config(void);
bool meiso_composition_host_bridge_private_load_config(
    const char *path,
    MeisoCompositionHostBridgePrivateConfig *out_config
);
bool meiso_composition_host_bridge_private_deadline_after_ms(
    int milliseconds,
    struct timespec *out_deadline
);
int meiso_composition_host_bridge_private_connect_numeric(
    const struct sockaddr_storage *address,
    socklen_t address_size
);
int meiso_composition_host_bridge_private_start_numeric_connect(
    const struct sockaddr_storage *address,
    socklen_t address_size,
    bool *out_connected
);
bool meiso_composition_host_bridge_private_time_reached(
    const struct timespec *when
);

bool meiso_composition_host_bridge_private_handle_command(
    MeisoCompositionHostBridgePrivateProcess *process,
    const char *line,
    size_t size
);

const char *meiso_composition_host_bridge_private_host_terminal_code(
    const MeisoCompositionHostBridgePrivateProcess *process
);
const char *meiso_composition_host_bridge_private_edge_status_code(
    MeisoEdgeStatus status
);
const char *meiso_composition_host_bridge_private_media_source_name(
    MeisoCompositionMediaSource value
);
const char *meiso_composition_host_bridge_private_pixel_format_name(
    MeisoCompositionMediaPixelFormat value
);
const char *meiso_composition_host_bridge_private_codec_name(
    MeisoCompositionMediaCodec value
);
bool meiso_composition_host_bridge_private_emit_line(const char *line);
bool meiso_composition_host_bridge_private_emit_error(
    uint64_t request_id,
    const char *code
);
bool meiso_composition_host_bridge_private_emit_receipt(
    uint64_t request_id,
    const MeisoEdgeCommandReceipt *receipt
);
bool meiso_composition_host_bridge_private_emit_snapshot(
    MeisoCompositionHostBridgePrivateProcess *process,
    uint64_t request_id
);
bool meiso_composition_host_bridge_private_emit_pump_result(
    const MeisoCompositionHostRuntimePumpResult *result
);
bool meiso_composition_host_bridge_private_emit_typed_truth(
    MeisoCompositionHostBridgePrivateProcess *process
);

bool meiso_composition_host_bridge_private_write_frame_output(
    MeisoCompositionHostBridgePrivateProcess *process,
    bool writable
);
bool meiso_composition_host_bridge_private_reconcile_media(
    MeisoCompositionHostBridgePrivateProcess *process
);
bool meiso_composition_host_bridge_private_finish_media_connect(
    MeisoCompositionHostBridgePrivateProcess *process
);
bool meiso_composition_host_bridge_private_drain_latest_frames(
    MeisoCompositionHostBridgePrivateProcess *process
);

bool meiso_composition_host_bridge_private_initialize_remote_process(
    MeisoCompositionHostBridgePrivateProcess *process,
    const MeisoCompositionHostBridgePrivateConfig *config,
    int frame_fd
);
bool meiso_composition_host_bridge_private_initialize_simulator_process(
    MeisoCompositionHostBridgePrivateProcess *process,
    int frame_fd
);
void meiso_composition_host_bridge_private_destroy_process(
    MeisoCompositionHostBridgePrivateProcess *process
);
bool meiso_composition_host_bridge_private_run_loop(
    MeisoCompositionHostBridgePrivateProcess *process
);

#endif
