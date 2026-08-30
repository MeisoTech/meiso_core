#include "meiso_four_kind_lab_private.h"

/* Connection orchestration owns Session/TCP progress and protocol close. */

static MeisoSessionConfig session_config(MeisoSessionRole role)
{
    MeisoSessionConfig config;
    const MeisoPeerId host_peer = {MEISO_LAB_HOST_PEER_ID_HIGH,
                                   MEISO_LAB_HOST_PEER_ID_LOW};
    const MeisoPeerId edge_peer = {MEISO_LAB_EDGE_PEER_ID_HIGH,
                                   MEISO_LAB_EDGE_PEER_ID_LOW};

    memset(&config, 0, sizeof(config));
    config.local_role = role;
    config.local_peer_id =
        role == MEISO_SESSION_ROLE_HOST ? host_peer : edge_peer;
    config.expected_peer_id =
        role == MEISO_SESSION_ROLE_HOST ? edge_peer : host_peer;
    config.protocol = (MeisoProtocolVersion){MEISO_LAB_PROTOCOL_MAJOR,
                                             MEISO_LAB_PROTOCOL_MINOR};
    config.profile.id =
        (MeisoProfileId){MEISO_LAB_PROFILE_ID_HIGH, MEISO_LAB_PROFILE_ID_LOW};
    config.profile.version = MEISO_LAB_PROFILE_VERSION;
    config.limits =
        (MeisoSessionLimits){MEISO_LAB_SESSION_MAX_FRAME_BYTES,
                             MEISO_LAB_SESSION_MAX_RUNTIME_PAYLOAD_BYTES,
                             MEISO_LAB_SESSION_MAX_REGISTRY_ENTRIES};
    return config;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_initialize_connection(
    MeisoFourKindLabConnection *connection, int fd, MeisoSessionRole role,
    const MeisoFourKindLabEdgeProcess *edge_process)
{
    MeisoPosixTcpConfig adapter_config;
    size_t required;

    if (connection == NULL || fd < 0 ||
        (role == MEISO_SESSION_ROLE_EDGE && edge_process == NULL)) {
        return false;
    }
    memset(&meiso_four_kind_lab_private_storage, 0,
           sizeof(meiso_four_kind_lab_private_storage));
    memset(connection, 0, sizeof(*connection));
    connection->fd = fd;
    connection->storage = &meiso_four_kind_lab_private_storage;
    connection->config = session_config(role);
    if (!meiso_four_kind_lab_private_random_nonce(
            &connection->config.local_nonce)) {
        return false;
    }
    if (role == MEISO_SESSION_ROLE_EDGE) {
        connection->config.local_incarnation = edge_process->scope.incarnation;
        connection->stream_epoch = edge_process->stream_epoch;
        connection->clock_epoch = edge_process->clock_epoch;
        connection->edge_scope = edge_process->scope;
    } else {
        if (!meiso_four_kind_lab_private_random_nonzero_u64(
                &connection->config.local_incarnation) ||
            !meiso_four_kind_lab_private_random_nonzero_u64(
                &connection->stream_epoch) ||
            !meiso_four_kind_lab_private_random_nonzero_u64(
                &connection->clock_epoch)) {
            return false;
        }
        connection->edge_scope = (MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE, 1U};
    }
    required = meiso_session_memory_required(&connection->config);
    if (required == 0U ||
        required > sizeof(connection->storage->session.bytes) ||
        meiso_session_init(connection->storage->session.bytes,
                           sizeof(connection->storage->session.bytes),
                           &connection->config,
                           &connection->session) != MEISO_SESSION_INIT_OK) {
        return false;
    }
    adapter_config.connected_fd = fd;
    adapter_config.session = connection->session;
    adapter_config.max_frame_bytes = MEISO_LAB_SESSION_MAX_FRAME_BYTES;
    adapter_config.max_io_bytes_per_step = MEISO_FOUR_KIND_LAB_IO_CHUNK_BYTES;
    required = meiso_posix_tcp_memory_required(&adapter_config);
    if (required == 0U ||
        required > sizeof(connection->storage->adapter.bytes) ||
        meiso_posix_tcp_init(connection->storage->adapter.bytes,
                             sizeof(connection->storage->adapter.bytes),
                             &adapter_config,
                             &connection->adapter) != MEISO_POSIX_TCP_INIT_OK) {
        return false;
    }
    meiso_lab_order_init(&connection->order);
    return meiso_session_start(connection->session) ==
           MEISO_SESSION_START_STARTED;
}

static bool write_status_ok(MeisoPosixTcpWriteStatus status)
{
    return status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
           status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
           status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
           status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
           status == MEISO_POSIX_TCP_WRITE_IDLE;
}

static bool read_status_progress(MeisoPosixTcpReadStatus status)
{
    return status == MEISO_POSIX_TCP_READ_PROGRESS ||
           status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
           status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
           status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS ||
           status == MEISO_POSIX_TCP_READ_PEER_CLOSED;
}

static void record_transport_failure(MeisoFourKindLabConnection *connection)
{
    if (connection != NULL &&
        connection->local_failure == MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_NONE) {
        connection->local_failure =
            MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_TRANSPORT_OR_TIMEOUT;
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_record_protocol_failure(
    MeisoFourKindLabConnection *connection)
{
    if (connection != NULL) {
        connection->local_failure = MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_PROTOCOL;
    }
}

static bool poll_adapter(MeisoFourKindLabConnection *connection,
                         const struct timespec *deadline,
                         MeisoSessionRuntimeMessage *out_runtime,
                         bool *out_received_runtime)
{
    struct pollfd descriptor;
    int result;
    int wait_ms;

    if (connection == NULL || deadline == NULL ||
        out_received_runtime == NULL) {
        return false;
    }
    *out_received_runtime = false;
    descriptor.fd = connection->fd;
    descriptor.events = 0;
    descriptor.revents = 0;
    if (meiso_posix_tcp_wants_read(connection->adapter)) {
        descriptor.events = (short)(descriptor.events | POLLIN);
    }
    if (meiso_posix_tcp_wants_write(connection->adapter)) {
        descriptor.events = (short)(descriptor.events | POLLOUT);
    }
    wait_ms = meiso_four_kind_lab_private_deadline_wait_ms(deadline);
    if (wait_ms == 0 || descriptor.events == 0) {
        record_transport_failure(connection);
        return false;
    }
    result = poll(&descriptor, (nfds_t)1U, wait_ms);
    if (result < 0 && errno == EINTR) {
        return true;
    }
    if (result < 0 || (descriptor.revents & POLLNVAL) != 0) {
        record_transport_failure(connection);
        return false;
    }
    if ((descriptor.revents & POLLOUT) != 0) {
        if (!write_status_ok(
                meiso_posix_tcp_on_writable(connection->adapter))) {
            record_transport_failure(connection);
            return false;
        }
    }
    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        MeisoSessionRuntimeMessage runtime;
        const MeisoPosixTcpReadStatus status =
            meiso_posix_tcp_on_readable(connection->adapter, &runtime);
        if (status == MEISO_POSIX_TCP_READ_RUNTIME) {
            if (out_runtime == NULL) {
                meiso_four_kind_lab_private_record_protocol_failure(connection);
                return false;
            }
            *out_runtime = runtime;
            *out_received_runtime = true;
        } else if (!read_status_progress(status)) {
            record_transport_failure(connection);
            return false;
        }
    }
    return true;
}

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_wait_session_ready(
    MeisoFourKindLabConnection *connection)
{
    struct timespec deadline;

    if (!meiso_four_kind_lab_private_make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!meiso_session_snapshot(connection->session, &snapshot)) {
            return false;
        }
        if (snapshot.state == MEISO_SESSION_STATE_READY &&
            snapshot.failure == MEISO_SESSION_FAILURE_NONE &&
            snapshot.remote_identity_known && snapshot.session_id_known &&
            meiso_peer_id_equal(snapshot.remote_peer_id,
                                connection->config.expected_peer_id)) {
            if (connection->config.local_role == MEISO_SESSION_ROLE_HOST) {
                connection->edge_scope.incarnation =
                    snapshot.remote_incarnation;
            }
            return true;
        }
        if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED ||
            !poll_adapter(connection, &deadline, &runtime, &received_runtime) ||
            received_runtime) {
            return false;
        }
    }
}

static bool flush_outbound(MeisoFourKindLabConnection *connection)
{
    struct timespec deadline;

    if (!meiso_four_kind_lab_private_make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!meiso_session_snapshot(connection->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED) {
            return false;
        }
        if (!snapshot.outbound_pending) {
            return true;
        }
        if (!poll_adapter(connection, &deadline, &runtime, &received_runtime) ||
            received_runtime) {
            return false;
        }
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_send_lab_message(
    MeisoFourKindLabConnection *connection, MeisoLabSender sender,
    const MeisoLabMessage *message)
{
    uint8_t encoded[MEISO_LAB_MAX_MESSAGE_SIZE];
    size_t encoded_size = 0U;
    const MeisoLabOrderStatus order_status =
        meiso_lab_order_advance(&connection->order, sender, message);

    if (order_status != MEISO_LAB_ORDER_ADVANCED &&
        order_status != MEISO_LAB_ORDER_COMPLETE) {
        meiso_four_kind_lab_private_record_protocol_failure(connection);
        return false;
    }
    if (meiso_lab_message_encode(message,
                                 (MeisoMutableBuffer){encoded, sizeof(encoded)},
                                 &encoded_size) != MEISO_LAB_CODEC_OK) {
        meiso_four_kind_lab_private_record_protocol_failure(connection);
        return false;
    }
    if (meiso_session_submit_runtime(connection->session,
                                     (MeisoByteView){encoded, encoded_size}) !=
        MEISO_SESSION_SUBMIT_QUEUED) {
        record_transport_failure(connection);
        return false;
    }
    return flush_outbound(connection);
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_receive_lab_message(
    MeisoFourKindLabConnection *connection, MeisoLabSender sender,
    MeisoLabMessage *out_message)
{
    struct timespec deadline;

    if (out_message == NULL ||
        !meiso_four_kind_lab_private_make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!poll_adapter(connection, &deadline, &runtime, &received_runtime)) {
            return false;
        }
        if (received_runtime) {
            const MeisoLabCodecStatus codec_status =
                meiso_lab_message_decode(runtime.payload, out_message);
            const MeisoLabOrderStatus order_status =
                codec_status == MEISO_LAB_CODEC_OK
                    ? meiso_lab_order_advance(&connection->order, sender,
                                              out_message)
                    : MEISO_LAB_ORDER_UNEXPECTED;
            if (codec_status != MEISO_LAB_CODEC_OK ||
                (order_status != MEISO_LAB_ORDER_ADVANCED &&
                 order_status != MEISO_LAB_ORDER_COMPLETE)) {
                meiso_four_kind_lab_private_record_protocol_failure(connection);
                return false;
            }
            return true;
        }
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE void
meiso_four_kind_lab_private_best_effort_protocol_close(
    MeisoFourKindLabConnection *connection)
{
    MeisoSessionSnapshot snapshot;

    if (connection == NULL || connection->session == NULL ||
        !meiso_session_snapshot(connection->session, &snapshot) ||
        snapshot.state == MEISO_SESSION_STATE_FAILED ||
        snapshot.state == MEISO_SESSION_STATE_CLOSED) {
        return;
    }
    if (meiso_session_close(connection->session,
                            connection->local_failure ==
                                    MEISO_FOUR_KIND_LAB_LOCAL_FAILURE_PROTOCOL
                                ? MEISO_SESSION_CLOSE_PROTOCOL_ERROR
                                : MEISO_SESSION_CLOSE_SHUTDOWN) ==
        MEISO_SESSION_CLOSE_QUEUED) {
        (void)flush_outbound(connection);
    }
}

MEISO_FOUR_KIND_LAB_PRIVATE bool meiso_four_kind_lab_private_normal_host_close(
    MeisoFourKindLabConnection *connection)
{
    return meiso_session_close(connection->session,
                               MEISO_SESSION_CLOSE_NORMAL) ==
               MEISO_SESSION_CLOSE_QUEUED &&
           flush_outbound(connection);
}

MEISO_FOUR_KIND_LAB_PRIVATE bool
meiso_four_kind_lab_private_edge_wait_peer_close(
    MeisoFourKindLabConnection *connection)
{
    struct timespec deadline;

    if (!meiso_four_kind_lab_private_make_deadline(&deadline)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        MeisoSessionRuntimeMessage runtime;
        bool received_runtime;

        if (!meiso_session_snapshot(connection->session, &snapshot)) {
            return false;
        }
        if (snapshot.state == MEISO_SESSION_STATE_CLOSED &&
            snapshot.failure == MEISO_SESSION_FAILURE_PEER_CLOSED &&
            snapshot.peer_close_reason_known &&
            snapshot.peer_close_reason == MEISO_SESSION_CLOSE_NORMAL) {
            return true;
        }
        if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
            !poll_adapter(connection, &deadline, &runtime, &received_runtime) ||
            received_runtime) {
            return false;
        }
    }
}
