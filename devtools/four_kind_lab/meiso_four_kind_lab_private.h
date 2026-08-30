#ifndef MEISO_FOUR_KIND_LAB_PRIVATE_H
#define MEISO_FOUR_KIND_LAB_PRIVATE_H

#define _POSIX_C_SOURCE 200809L

#include <meiso/adapters/posix/tcp.h>
#include <meiso/core/blob.h>
#include <meiso/core/descriptor_set.h>
#include <meiso/core/session.h>
#include <meiso/core/state.h>
#include <meiso/core/stream.h>
#include <meiso/internal/lab/four_kind_protocol.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Build-private and non-installed. Process storage and self-test observation
 * are owned for process life. A connection borrows its fd and storage; Core
 * primitives borrow their caller arenas. MeisoFourKindLabThermalChild owns a
 * child pid until reap/termination. All orchestration is single-threaded and
 * non-reentrant.
 */
#if defined(__GNUC__) || defined(__clang__)
#define MEISO_FOUR_KIND_LAB_PRIVATE __attribute__((visibility("hidden")))
#else
#define MEISO_FOUR_KIND_LAB_PRIVATE
#endif

enum {
    MEISO_FOUR_KIND_LAB_TCP_PORT = MEISO_LAB_TCP_PORT,
    MEISO_FOUR_KIND_LAB_MAX_SESSIONS = 16,
    MEISO_FOUR_KIND_LAB_PHASE_TIMEOUT_MS = 5000,
    MEISO_FOUR_KIND_LAB_THERMAL_PREFLIGHT_TIMEOUT_MS = 1000,
    MEISO_FOUR_KIND_LAB_POLL_SLICE_MS = 50,
    MEISO_FOUR_KIND_LAB_IO_CHUNK_BYTES = 1024,
    MEISO_FOUR_KIND_LAB_ARENA_BYTES = 4096,
    MEISO_FOUR_KIND_LAB_CONFIG_LINE_BYTES = 256
};

MEISO_FOUR_KIND_LAB_PRIVATE extern const char
    meiso_four_kind_lab_private_address_env[];
MEISO_FOUR_KIND_LAB_PRIVATE extern const char
    meiso_four_kind_lab_private_expected_edge[];

typedef union {
    max_align_t alignment;
    uint8_t bytes[MEISO_FOUR_KIND_LAB_ARENA_BYTES];
} MeisoFourKindLabArena;

typedef struct {
    MeisoFourKindLabArena session;
    MeisoFourKindLabArena adapter;
    MeisoFourKindLabArena descriptors;
    MeisoFourKindLabArena state;
    MeisoFourKindLabArena stream;
    MeisoFourKindLabArena action;
    MeisoFourKindLabArena blob;
} MeisoFourKindLabStorage;

typedef uint8_t MeisoFourKindLabTest;
enum {
    MEISO_FOUR_KIND_LAB_TEST_STATE = 1,
    MEISO_FOUR_KIND_LAB_TEST_STREAM = 2,
    MEISO_FOUR_KIND_LAB_TEST_ACTION = 3,
    MEISO_FOUR_KIND_LAB_TEST_BLOB = 4,
    MEISO_FOUR_KIND_LAB_TEST_ALL = 5
};

typedef uint8_t MeisoFourKindLabWifiOperstate;
enum {
    MEISO_FOUR_KIND_WIFI_OPERSTATE_UNKNOWN = 0,
    MEISO_FOUR_KIND_WIFI_OPERSTATE_DOWN = 1,
    MEISO_FOUR_KIND_WIFI_OPERSTATE_DORMANT = 2,
    MEISO_FOUR_KIND_WIFI_OPERSTATE_LOWERLAYERDOWN = 3,
    MEISO_FOUR_KIND_WIFI_OPERSTATE_NOTPRESENT = 4,
    MEISO_FOUR_KIND_WIFI_OPERSTATE_TESTING = 5,
    MEISO_FOUR_KIND_WIFI_OPERSTATE_UP = 6
};

typedef struct {
    bool interface_present;
    bool carrier;
    MeisoFourKindLabWifiOperstate operstate;
    bool signal_present;
    int32_t signal_mbm;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_dropped;
    uint64_t tx_dropped;
} MeisoFourKindLabNetworkReading;

typedef struct {
    bool present;
    int32_t corrected_millic;
    uint32_t uncertainty_millic;
} MeisoFourKindLabThermalReading;

typedef struct {
    pid_t pid;
    bool pending;
} MeisoFourKindLabThermalChild;

typedef uint8_t MeisoFourKindLabThermalChildMode;
enum {
    MEISO_FOUR_KIND_THERMAL_CHILD_ACTUAL = 1,
    MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_PRESENT = 2,
    MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_ABSENT = 3,
    MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_TIMEOUT = 4,
    MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_PARTIAL = 5,
    MEISO_FOUR_KIND_THERMAL_CHILD_FIXTURE_EARLY_EXIT = 6
};

typedef uint8_t MeisoFourKindLabThermalPreflightStatus;
enum {
    MEISO_FOUR_KIND_THERMAL_PREFLIGHT_PRESENT = 1,
    MEISO_FOUR_KIND_THERMAL_PREFLIGHT_ABSENT = 2,
    MEISO_FOUR_KIND_THERMAL_PREFLIGHT_TIMEOUT = 3,
    MEISO_FOUR_KIND_THERMAL_PREFLIGHT_CHILD_ERROR = 4,
    MEISO_FOUR_KIND_THERMAL_PREFLIGHT_LOCAL_ERROR = 5
};

typedef struct {
    MeisoFourKindLabNetworkReading baseline;
    uint64_t started_ns;
    uint64_t window_id;
    MeisoFourKindLabThermalReading thermal;
} MeisoFourKindLabTelemetryEnvironment;

typedef struct {
    struct in_addr address;
    uint16_t port;
} MeisoFourKindLabHostLocator;

typedef uint8_t MeisoFourKindLabLocalFailure;
enum {
    MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_NONE = 0,
    MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_TRANSPORT_OR_TIMEOUT = 1,
    MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_PROTOCOL = 2
};

typedef struct {
    int fd;
    MeisoSessionConfig config;
    MeisoSession *session;
    MeisoPosixTcp *adapter;
    MeisoLabOrder order;
    MeisoOwnerScope edge_scope;
    uint64_t stream_epoch;
    uint64_t clock_epoch;
    MeisoFourKindLabStorage *storage;
    MeisoFourKindLabLocalFailure local_failure;
} MeisoFourKindLabConnection;

typedef struct {
    MeisoDescriptorSet *descriptors;
    MeisoStateReplica *state;
    MeisoStream *stream;
    MeisoActionCaller *action;
    MeisoBlobStore *blob;
    MeisoDeliveryGeneration action_delivery;
    MeisoActionId action_id;
    MeisoOwnerScope edge_owner;
    MeisoLabEdgeState edge_state;
    bool edge_state_known;
    bool action_delivery_accepted;
    bool action_terminal;
} MeisoFourKindLabHostPrimitives;

typedef struct {
    MeisoStatePublisher *state;
    MeisoDeliveryGeneration state_delivery;
    MeisoOwnerScope state_owner;
    uint64_t state_version;
    bool state_delivery_active;
} MeisoFourKindLabEdgePrimitives;

typedef struct {
    MeisoOwnerScope scope;
    uint64_t stream_epoch;
    uint64_t clock_epoch;
    uint64_t state_version;
    uint64_t stream_sequence;
    MeisoFourKindLabTelemetryEnvironment telemetry;
    MeisoFourKindLabThermalChild thermal_child;
    MeisoFourKindLabArena action_arena;
    MeisoActionExecutor *action;
    MeisoActionId active_action_id;
    bool active_action_nonterminal;
} MeisoFourKindLabEdgeProcess;

typedef struct {
    MeisoContentId expected_id;
    MeisoByteView expected_content;
    size_t calls;
} MeisoFourKindLabBlobVerifyContext;

MEISO_FOUR_KIND_LAB_PRIVATE extern MeisoFourKindLabStorage
    meiso_four_kind_lab_private_storage;

#ifdef MEISO_LAB_INTERNAL_TEST
enum { MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS = 2 };

typedef struct {
    bool enabled;
    bool fixed_action_id;
    size_t session_count;
    MeisoSessionId session_ids[MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS];
    MeisoOwnerScope owners[MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS];
    uint64_t state_versions[MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS];
    uint64_t stream_epochs[MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS];
    uint64_t stream_sequences[MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS];
    uint64_t clock_epochs[MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS];
    uint64_t action_windows[MEISO_FOUR_KIND_LAB_SELF_TEST_SESSIONS];
} MeisoFourKindLabSelfTestObservation;

MEISO_FOUR_KIND_LAB_PRIVATE extern MeisoFourKindLabSelfTestObservation
    meiso_four_kind_lab_private_self_test;
#endif

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_accept_edge(int listener);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_best_effort_protocol_close(
    MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_bytes_equal(MeisoByteView lhs, MeisoByteView rhs);

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_capture_telemetry(
    MeisoFourKindLabTelemetryEnvironment *environment,
    MeisoLabEdgeState *out_state, MeisoLabTelemetrySample *out_sample);

MEISO_FOUR_KIND_LAB_PRIVATE void meiso_four_kind_lab_private_close_fd(int *fd);

MEISO_FOUR_KIND_LAB_PRIVATE int meiso_four_kind_lab_private_connect_host(
    const MeisoFourKindLabHostLocator *locator);

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_deadline_wait_ms(const struct timespec *deadline);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_edge_fail_active_state_delivery(
    MeisoFourKindLabEdgePrimitives *edge);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_edge_mark_active_action_indeterminate(
    MeisoFourKindLabEdgeProcess *process);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_edge_wait_peer_close(
    MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_action_delivery_accepted(void);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_action_event(uint64_t new_window_id);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_action_executor_accepted(void);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_blob_event(size_t content_size);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_blob_staged(size_t content_size);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_connection_failed(
    const MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_registry(void);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_session_closed(
    const MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_session_failed(void);

MEISO_FOUR_KIND_LAB_PRIVATE void meiso_four_kind_lab_private_emit_session_ready(
    const MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_state_event(uint64_t version,
                                             const MeisoLabEdgeState *state);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_stream_consumed(
    const MeisoLabStreamMetrics *metrics);

MEISO_FOUR_KIND_LAB_PRIVATE void meiso_four_kind_lab_private_emit_stream_event(
    const MeisoLabStreamMetrics *metrics, uint64_t receive_time_ns);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_transcript_failed(
    const MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_emit_verdicts(MeisoFourKindLabTest test,
                                          bool passed);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_host_mark_accepted_action_indeterminate(
    MeisoFourKindLabHostPrimitives *host);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_host_runtimely_state_actual(
    MeisoFourKindLabConnection *connection,
    MeisoFourKindLabHostPrimitives *host, const MeisoLabMessage *received,
    MeisoStateView *out_current);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_connection(
    MeisoFourKindLabConnection *connection, int fd, MeisoSessionRole role,
    const MeisoFourKindLabEdgeProcess *edge_process);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_edge_primitives(
    MeisoFourKindLabConnection *connection,
    MeisoFourKindLabEdgePrimitives *edge);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_edge_process(
    MeisoFourKindLabEdgeProcess *process);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_edge_process_with_thermal_mode(
    MeisoFourKindLabEdgeProcess *process,
    MeisoFourKindLabThermalChildMode thermal_mode);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_telemetry(
    MeisoFourKindLabTelemetryEnvironment *environment,
    MeisoFourKindLabThermalChild *thermal_child,
    MeisoFourKindLabThermalChildMode thermal_mode);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_make_deadline(struct timespec *out_deadline);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_make_deadline_after_ms(
    struct timespec *out_deadline, unsigned int timeout_ms);

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_make_listener(uint16_t port);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_make_thermal_pipe(int descriptors[2]);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_monotonic_now_ns(uint64_t *out_value);

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_normal_host_close(
    MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_parse_host_config(
    const char *path, MeisoFourKindLabHostLocator *out_locator);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_parse_port(const char *value, uint16_t *out_port);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_parse_u64_text(const char *text_value,
                                           uint64_t *out_value);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_random_action_id(MeisoActionId *out_id);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_random_nonce(MeisoSessionNonce *out_nonce);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_random_nonzero_u64(uint64_t *out_value);

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_read_network(
    MeisoFourKindLabNetworkReading *out_reading);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_read_text_file(const char *path, char *output,
                                           size_t capacity);

MEISO_FOUR_KIND_LAB_PRIVATE uint64_t
meiso_four_kind_lab_private_read_u64_be(const uint8_t *source);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_read_u64_file(const char *path,
                                          uint64_t *out_value);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_receive_lab_message(
    MeisoFourKindLabConnection *connection, MeisoLabSender sender,
    MeisoLabMessage *out_message);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_record_protocol_failure(
    MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_reset_telemetry_window(
    MeisoFourKindLabTelemetryEnvironment *environment);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_run_edge_accept_loop(
    int listener, unsigned int sessions, MeisoFourKindLabEdgeProcess *process);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_run_edge_transcript(
    int fd, MeisoFourKindLabEdgeProcess *process);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_run_host_transcript(int fd,
                                                MeisoFourKindLabTest test);

MEISO_FOUR_KIND_LAB_PRIVATE int
meiso_four_kind_lab_private_run_internal_self_test(void);

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_send_lab_message(
    MeisoFourKindLabConnection *connection, MeisoLabSender sender,
    const MeisoLabMessage *message);

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_set_nonblocking_cloexec(int fd);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_thermal_child_reap_nonblocking(
    MeisoFourKindLabThermalChild *child);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_thermal_child_terminate_nonblocking(
    MeisoFourKindLabThermalChild *child);

MEISO_FOUR_KIND_LAB_PRIVATE MeisoFourKindLabThermalPreflightStatus
meiso_four_kind_lab_private_thermal_preflight_bounded(
    MeisoFourKindLabThermalReading *out_reading,
    MeisoFourKindLabThermalChild *child, MeisoFourKindLabThermalChildMode mode);

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_verify_fixed_blob(
    const MeisoContentId *expected_content_id, MeisoByteView staged_content,
    void *context);

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_wait_session_ready(
    MeisoFourKindLabConnection *connection);

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_write_u64_be(uint8_t *destination, uint64_t value);

#endif
