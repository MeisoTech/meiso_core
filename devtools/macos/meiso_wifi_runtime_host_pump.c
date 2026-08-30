#define _POSIX_C_SOURCE 200809L

#include "meiso_wifi_runtime_host_private.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>

/* Pumps Session/TCP readiness, runtime events, and interactive stdin. */

static bool write_status_progress(MeisoPosixTcpWriteStatus status)
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
        status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS;
}

bool meiso_wifi_runtime_host_private_flush_outbound(MeisoWifiRuntimeHostPrivate *host)
{
    struct timespec deadline;

    if (host == NULL || host->adapter == NULL || host->session == NULL ||
        !meiso_wifi_runtime_host_private_deadline_after_ms(&deadline, MEISO_WIFI_RUNTIME_HOST_PRIVATE_PHASE_TIMEOUT_MS)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptor;
        int result;
        int wait_ms;

        if (!meiso_session_snapshot(host->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED) {
            return false;
        }
        if (!snapshot.outbound_pending) {
            return true;
        }
        wait_ms = meiso_wifi_runtime_host_private_deadline_wait_ms(&deadline);
        if (wait_ms == 0) {
            return false;
        }
        descriptor.fd = host->fd;
        descriptor.events = POLLOUT;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0 ||
            ((descriptor.revents & (POLLERR | POLLHUP)) != 0 &&
             (descriptor.revents & POLLOUT) == 0) ||
            ((descriptor.revents & POLLOUT) != 0 && !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            ))) {
            return false;
        }
    }
}

bool meiso_wifi_runtime_host_private_wait_session_ready(MeisoWifiRuntimeHostPrivate *host, MeisoSessionSnapshot *out_snapshot)
{
    struct timespec deadline;

    if (host == NULL || out_snapshot == NULL ||
        !meiso_wifi_runtime_host_private_deadline_after_ms(&deadline, MEISO_WIFI_RUNTIME_HOST_PRIVATE_PHASE_TIMEOUT_MS)) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptor;
        short events = 0;
        int result;
        int wait_ms;

        if (!meiso_session_snapshot(host->session, &snapshot)) {
            return false;
        }
        if (snapshot.state == MEISO_SESSION_STATE_READY &&
            snapshot.failure == MEISO_SESSION_FAILURE_NONE &&
            snapshot.remote_identity_known && snapshot.session_id_known &&
            meiso_peer_id_equal(
                snapshot.remote_peer_id,
                host->bootstrap.expected_edge_peer_id
            )) {
            *out_snapshot = snapshot;
            return true;
        }
        if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            return false;
        }
        if (meiso_posix_tcp_wants_read(host->adapter)) {
            events = (short)(events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(host->adapter)) {
            events = (short)(events | POLLOUT);
        }
        wait_ms = meiso_wifi_runtime_host_private_deadline_wait_ms(&deadline);
        if (wait_ms == 0 || events == 0) {
            return false;
        }
        descriptor.fd = host->fd;
        descriptor.events = events;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0) {
            return false;
        }
        if ((descriptor.revents & POLLOUT) != 0 && !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            )) {
            return false;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            MeisoSessionRuntimeMessage runtime_message;
            const MeisoPosixTcpReadStatus read_status =
                meiso_posix_tcp_on_readable(
                    host->adapter, &runtime_message
                );
            if (!read_status_progress(read_status)) {
                return false;
            }
        }
    }
}

bool meiso_wifi_runtime_host_private_wait_runtime_event(
    MeisoWifiRuntimeHostPrivate *host,
    const struct timespec *deadline,
    MeisoRuntimeEvent *out_event
)
{
    if (host == NULL || deadline == NULL || out_event == NULL) {
        return false;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptor;
        short events = 0;
        int result;
        int wait_ms;

        if (!meiso_session_snapshot(host->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            return false;
        }
        if (meiso_posix_tcp_wants_read(host->adapter)) {
            events = (short)(events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(host->adapter)) {
            events = (short)(events | POLLOUT);
        }
        wait_ms = meiso_wifi_runtime_host_private_deadline_wait_ms(deadline);
        if (wait_ms == 0 || events == 0) {
            return false;
        }
        descriptor.fd = host->fd;
        descriptor.events = events;
        descriptor.revents = 0;
        result = poll(&descriptor, (nfds_t)1U, wait_ms);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 || (descriptor.revents & POLLNVAL) != 0) {
            return false;
        }
        if ((descriptor.revents & POLLOUT) != 0 && !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            )) {
            return false;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            MeisoSessionRuntimeMessage message;
            const MeisoPosixTcpReadStatus read_status =
                meiso_posix_tcp_on_readable(host->adapter, &message);

            if (read_status == MEISO_POSIX_TCP_READ_RUNTIME) {
                return meiso_runtime_receive(
                    host->runtime, &message, out_event
                ) == MEISO_RUNTIME_OK;
            }
            if (!read_status_progress(read_status)) {
                return false;
            }
        }
    }
}

MeisoWifiRuntimeHostPrivateInteractiveItemType meiso_wifi_runtime_host_private_wait_interactive_item(
    MeisoWifiRuntimeHostPrivate *host,
    MeisoWifiRuntimeHostPrivateControlInput *input,
    MeisoWifiRuntimeHostPrivateControlCommand *out_command,
    MeisoRuntimeEvent *out_event
)
{
    if (host == NULL || input == NULL || out_command == NULL ||
        out_event == NULL) {
        return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
    }
    for (;;) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptors[2];
        bool command_present = false;
        int result;

        if (!meiso_wifi_runtime_host_private_extract_control_command(
                input, out_command, &command_present
            )) {
            return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
        }
        if (command_present) {
            return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_COMMAND;
        }
        if (!meiso_session_snapshot(host->session, &snapshot) ||
            snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
        }
        descriptors[0].fd = host->fd;
        descriptors[0].events = 0;
        descriptors[0].revents = 0;
        if (meiso_posix_tcp_wants_read(host->adapter)) {
            descriptors[0].events = (short)(
                descriptors[0].events | POLLIN
            );
        }
        if (meiso_posix_tcp_wants_write(host->adapter)) {
            descriptors[0].events = (short)(
                descriptors[0].events | POLLOUT
            );
        }
        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        result = poll(descriptors, (nfds_t)2U, -1);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0 ||
            (descriptors[0].revents & POLLNVAL) != 0 ||
            (descriptors[1].revents & (POLLERR | POLLNVAL)) != 0) {
            return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
        }
        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0) {
            const ssize_t count = read(
                STDIN_FILENO,
                input->bytes + input->size,
                sizeof(input->bytes) - input->size
            );
            if (count > 0) {
                input->size += (size_t)count;
                continue;
            }
            if (count < 0 &&
                (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
            return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
        }
        if ((descriptors[0].revents & POLLOUT) != 0 &&
            !write_status_progress(
                meiso_posix_tcp_on_writable(host->adapter)
            )) {
            return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
        }
        if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            MeisoSessionRuntimeMessage message;
            const MeisoPosixTcpReadStatus read_status =
                meiso_posix_tcp_on_readable(host->adapter, &message);

            if (read_status == MEISO_POSIX_TCP_READ_RUNTIME) {
                if (meiso_runtime_receive(
                        host->runtime, &message, out_event
                    ) != MEISO_RUNTIME_OK) {
                    return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
                }
                return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_RUNTIME;
            }
            if (!read_status_progress(read_status)) {
                return MEISO_WIFI_RUNTIME_HOST_PRIVATE_INTERACTIVE_ITEM_FAILED;
            }
        }
    }
}
