#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_wifi_runtime_edge_private.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>

/*
 * Owns the nonblocking transport poll pump and bounded local-close flush. It
 * observes process stop state but never owns configuration or Runtime truth.
 */

static bool source_loss_drained(const K230dWifiRuntimeEdge *edge)
{
    return edge->source_continuity_lost &&
        !edge->pending_sample.present && !edge->pending_accept.present &&
        meiso_k230d_wifi_runtime_edge_private_session_tx_idle(edge);
}

static int poll_timeout_ms(const K230dWifiRuntimeEdge *edge, uint64_t now_ns)
{
    uint64_t target =
        meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
            now_ns, K230D_WIFI_RUNTIME_EDGE_POLL_SLICE_MS
        );
    uint64_t remaining;

    if (meiso_posix_tcp_wants_write(edge->transport)) {
        return 0;
    }
    if (edge->production_active && !edge->pending_sample.present &&
        edge->next_sample_ns < target) {
        target = edge->next_sample_ns;
    }
    if (edge->desired_profile_valid && edge->desired_enabled &&
        !edge->production_active && edge->next_probe_ns < target) {
        target = edge->next_probe_ns;
    }
    if (target <= now_ns) {
        return 0;
    }
    remaining = target - now_ns;
    remaining = (remaining + UINT64_C(999999)) / UINT64_C(1000000);
    return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

static bool write_status_ok(MeisoPosixTcpWriteStatus status)
{
    return status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
        status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
        status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
        status == MEISO_POSIX_TCP_WRITE_IDLE;
}

static bool peer_close_is_normal_and_stopped(K230dWifiRuntimeEdge *edge)
{
    MeisoSessionSnapshot snapshot;
    return !edge->source_continuity_lost &&
        meiso_session_snapshot(edge->session, &snapshot) &&
        snapshot.state == MEISO_SESSION_STATE_CLOSED &&
        snapshot.failure == MEISO_SESSION_FAILURE_PEER_CLOSED &&
        snapshot.peer_close_reason_known &&
        snapshot.peer_close_reason == MEISO_SESSION_CLOSE_NORMAL &&
        !edge->production_active && !edge->pending_sample.present &&
        edge->active_production_epoch == 0U &&
        !edge->pending_accept.present && !edge->actual_in_flight &&
        !edge->actual_outbound.present && !edge->actual_next.present &&
        edge->current_desired.present && !edge->desired_enabled &&
        edge->production_applied_desired.present &&
        meiso_owner_scope_equal(
            edge->production_applied_desired.owner,
            edge->current_desired.owner
        ) && edge->production_applied_desired.version ==
            edge->current_desired.version;
}

static bool process_readable(K230dWifiRuntimeEdge *edge, uint64_t now_ns)
{
    MeisoSessionRuntimeMessage message;
    const MeisoPosixTcpReadStatus status = meiso_posix_tcp_on_readable(
        edge->transport, &message
    );

    switch (status) {
    case MEISO_POSIX_TCP_READ_PROGRESS:
    case MEISO_POSIX_TCP_READ_WOULD_BLOCK:
    case MEISO_POSIX_TCP_READ_INTERRUPTED:
    case MEISO_POSIX_TCP_READ_SESSION_PROGRESS:
        return true;
    case MEISO_POSIX_TCP_READ_RUNTIME:
        return edge->runtime_initialized &&
            meiso_k230d_wifi_runtime_edge_private_handle_runtime_message(edge, &message, now_ns);
    case MEISO_POSIX_TCP_READ_PEER_CLOSED:
        edge->peer_closed_normally = peer_close_is_normal_and_stopped(edge);
        return edge->peer_closed_normally;
    default:
        return false;
    }
}

static bool phase_timed_out(const K230dWifiRuntimeEdge *edge, uint64_t now_ns)
{
    if (!edge->runtime_initialized) {
        return now_ns >= edge->phase_deadline_ns;
    }
    return !edge->catalog_active && now_ns >= edge->phase_deadline_ns;
}

bool meiso_k230d_wifi_runtime_edge_private_run_connection(K230dWifiRuntimeEdge *edge)
{
    uint64_t now_ns;

    if (!meiso_k230d_wifi_runtime_edge_private_monotonic_now_ns(&now_ns)) {
        return false;
    }
    edge->phase_deadline_ns = meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
        now_ns, K230D_WIFI_RUNTIME_EDGE_PHASE_TIMEOUT_MS
    );
    while (!meiso_k230d_wifi_runtime_edge_private_g_stop_requested &&
           !edge->peer_closed_normally) {
        struct pollfd descriptor;
        MeisoSessionSnapshot snapshot;
        int poll_result;

        if (!meiso_k230d_wifi_runtime_edge_private_monotonic_now_ns(
                &now_ns
            ) || phase_timed_out(edge, now_ns) ||
            !meiso_session_snapshot(edge->session, &snapshot)) {
            return false;
        }
        if (!edge->runtime_initialized &&
            snapshot.state == MEISO_SESSION_STATE_READY) {
            if (!meiso_k230d_wifi_runtime_edge_private_initialize_runtime(
                    edge, now_ns
                )) {
                return false;
            }
        }
        if (!meiso_k230d_wifi_runtime_edge_private_drive_application(edge, now_ns)) {
            return false;
        }
        if (source_loss_drained(edge)) {
            return false;
        }
        descriptor.fd = edge->connection_fd;
        descriptor.events = 0;
        descriptor.revents = 0;
        if (!edge->source_continuity_lost &&
            meiso_posix_tcp_wants_read(edge->transport)) {
            descriptor.events = (short)(descriptor.events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(edge->transport)) {
            descriptor.events = (short)(descriptor.events | POLLOUT);
        }
        poll_result = poll(
            &descriptor,
            (nfds_t)1U,
            poll_timeout_ms(edge, now_ns)
        );
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if ((descriptor.revents & POLLOUT) != 0 && !write_status_ok(
                meiso_posix_tcp_on_writable(edge->transport)
            )) {
            return false;
        }
        if (!edge->source_continuity_lost &&
            (descriptor.revents & POLLIN) != 0 &&
            !process_readable(edge, now_ns)) {
            return false;
        }
        if (!edge->source_continuity_lost &&
            (descriptor.revents & POLLHUP) != 0 &&
            (descriptor.revents & POLLIN) == 0 &&
            meiso_posix_tcp_wants_read(edge->transport) &&
            !process_readable(edge, now_ns)) {
            return false;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            meiso_posix_tcp_abort(edge->transport, EIO);
            return false;
        }
    }
    return edge->peer_closed_normally;
}

void meiso_k230d_wifi_runtime_edge_private_flush_local_close(
    K230dWifiRuntimeEdge *edge,
    MeisoSessionCloseReason reason
)
{
    uint64_t now_ns;
    uint64_t deadline;
    MeisoSessionCloseStatus close_status;

    if (edge->session == NULL || edge->transport == NULL ||
        !meiso_k230d_wifi_runtime_edge_private_monotonic_now_ns(&now_ns)) {
        return;
    }
    close_status = meiso_session_close(edge->session, reason);
    if (close_status != MEISO_SESSION_CLOSE_QUEUED) {
        return;
    }
    deadline = meiso_k230d_wifi_runtime_edge_private_deadline_after_ms(
        now_ns, K230D_WIFI_RUNTIME_EDGE_CLOSE_TIMEOUT_MS
    );
    while (meiso_posix_tcp_wants_write(edge->transport) &&
           meiso_k230d_wifi_runtime_edge_private_monotonic_now_ns(
               &now_ns
           ) && now_ns < deadline) {
        struct pollfd descriptor = {
            edge->connection_fd,
            POLLOUT,
            0
        };
        const int wait_ms = (int)((deadline - now_ns) /
            UINT64_C(1000000));
        const int result = poll(
            &descriptor,
            (nfds_t)1U,
            wait_ms > K230D_WIFI_RUNTIME_EDGE_POLL_SLICE_MS ?
                K230D_WIFI_RUNTIME_EDGE_POLL_SLICE_MS : wait_ms
        );
        if (result > 0 && (descriptor.revents & POLLOUT) != 0) {
            if (!write_status_ok(
                    meiso_posix_tcp_on_writable(edge->transport)
                )) {
                break;
            }
        } else if (result < 0 && errno != EINTR) {
            break;
        }
    }
}
