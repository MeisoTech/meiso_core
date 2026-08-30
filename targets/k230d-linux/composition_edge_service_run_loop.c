#define _POSIX_C_SOURCE 200809L

#include "meiso_k230d_composition_edge_service_private.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

/*
 * Owns readiness polling, speculative media-candidate validation, and one
 * bounded Edge Runtime pump step per loop turn. It does not own teardown.
 */

/*
 * A media peer may retire a speculative connection while this runner is
 * waiting for the matching control-plane Actuals to become accepted. Probe
 * that caller-owned candidate without consuming OPEN bytes. POLLHUP/POLLERR
 * is terminal even when unread bytes remain; a positive MSG_PEEK is otherwise
 * a live candidate, and EAGAIN/EINTR means that no terminal condition was
 * observed. The socket is nonblocking before it reaches this function.
 */
PendingMediaCandidateStatus
meiso_k230d_composition_edge_service_private_pending_media_candidate_status(
    int fd,
    short revents
)
{
    uint8_t byte;
    ssize_t received;

    if (fd < 0 || (revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        return PENDING_MEDIA_CANDIDATE_CLOSED;
    }
    received = recv(fd, &byte, sizeof(byte), MSG_PEEK);
    if (received > 0) {
        return PENDING_MEDIA_CANDIDATE_LIVE;
    }
    if (received == 0) {
        return PENDING_MEDIA_CANDIDATE_CLOSED;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ?
        PENDING_MEDIA_CANDIDATE_LIVE : PENDING_MEDIA_CANDIDATE_CLOSED;
}

static bool media_connection_step(K230dCompositionEdgeService *service)
{
    MeisoK230dCompositionEdgeRuntimeSnapshot snapshot;
    if (!meiso_k230d_composition_edge_runtime_snapshot(
            service->edge_runtime, &snapshot
        )) {
        return false;
    }
    if (service->media_bound && !snapshot.media.bound) {
        service->media_bound = false;
        meiso_k230d_composition_edge_service_private_close_fd(
            &service->media_fd
        );
        (void)printf(
            "{\"type\":\"media\",\"phase\":\"close\","
            "\"status\":\"BOUNDARY\"}\n"
        );
        (void)fflush(stdout);
    }
    return true;
}

static bool media_candidate_bind_step(K230dCompositionEdgeService *service)
{
    if (service->media_fd >= 0 && !service->media_bound) {
        MeisoK230dCompositionEdgeRuntimeMediaBindReceipt receipt;
        const MeisoK230dCompositionEdgeRuntimeStatus status =
            meiso_k230d_composition_edge_runtime_bind_media_fd(
                service->edge_runtime, service->media_fd, &receipt
            );
        if (status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK) {
            service->media_bound = true;
            (void)printf(
                "{\"type\":\"media\",\"phase\":\"bind\","
                "\"status\":\"READY\",\"slot\":%u,\"mask\":%u}\n",
                (unsigned)receipt.slot_index,
                (unsigned)receipt.active_route_mask
            );
            (void)fflush(stdout);
        } else if (
            status != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_NOT_READY &&
            status != MEISO_K230D_COMPOSITION_EDGE_RUNTIME_BUSY
        ) {
            meiso_k230d_composition_edge_service_private_close_fd(
                &service->media_fd
            );
            if (status == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_CAPACITY) {
                meiso_k230d_composition_edge_service_private_close_fd(
                    &service->media_listener
                );
            }
        }
    }
    return true;
}

bool meiso_k230d_composition_edge_service_private_run_loop(
    K230dCompositionEdgeService *service
)
{
    while (!meiso_k230d_composition_edge_service_private_g_stop_requested) {
        MeisoK230dCompositionEdgeRuntimeIoInterest interest;
        MeisoK230dCompositionEdgeRuntimeIoReady ready;
        MeisoK230dCompositionEdgeRuntimePumpResult result;
        struct pollfd descriptors[3];
        nfds_t descriptor_count = 0U;
        int control_index = -1;
        int listener_index = -1;
        int media_index = -1;
        uint64_t now_ns;
        int poll_status;
        MeisoK230dCompositionEdgeRuntimePumpStatus pump_status;

        if (!meiso_k230d_composition_edge_service_private_monotonic_now_ns(
                &now_ns
            ) || !meiso_k230d_composition_edge_service_private_collect_thermal_worker(
                service, now_ns
            ) || !media_connection_step(service) ||
            !meiso_k230d_composition_edge_runtime_readiness(
                service->edge_runtime, &interest
            )) {
            return false;
        }
        memset(descriptors, 0, sizeof(descriptors));
        control_index = (int)descriptor_count;
        descriptors[descriptor_count].fd = service->control_fd;
        descriptors[descriptor_count].events = (short)(
            (interest.control_read ? POLLIN : 0) |
            (interest.control_write ? POLLOUT : 0)
        );
        ++descriptor_count;
        if (service->media_listener >= 0 && service->media_fd < 0) {
            listener_index = (int)descriptor_count;
            descriptors[descriptor_count].fd = service->media_listener;
            descriptors[descriptor_count].events = POLLIN;
            ++descriptor_count;
        }
        if (service->media_fd >= 0) {
            media_index = (int)descriptor_count;
            descriptors[descriptor_count].fd = service->media_fd;
            descriptors[descriptor_count].events = service->media_bound ?
                (short)((interest.media_read ? POLLIN : 0) |
                        (interest.media_write ? POLLOUT : 0)) :
                POLLIN;
            ++descriptor_count;
        }
        poll_status = poll(
            descriptors, descriptor_count, POLL_SLICE_MS
        );
        if (poll_status < 0 && errno != EINTR) {
            return false;
        }
        if (listener_index >= 0 &&
            (descriptors[listener_index].revents & POLLIN) != 0) {
            const int accepted =
                meiso_k230d_composition_edge_service_private_accept_connected(
                    service->media_listener, false
                );
            if (accepted >= 0) {
                service->media_fd = accepted;
                (void)printf(
                    "{\"type\":\"media\",\"phase\":\"accept\","
                    "\"status\":\"CONNECTED\"}\n"
                );
                (void)fflush(stdout);
            } else if (accepted != -2) {
                return false;
            }
        }
        if (!meiso_k230d_composition_edge_service_private_monotonic_now_ns(
                &now_ns
            )) {
            return false;
        }
        memset(&ready, 0, sizeof(ready));
        if ((control_index >= 0 &&
             (descriptors[control_index].revents & POLLNVAL) != 0) ||
            (listener_index >= 0 &&
             (descriptors[listener_index].revents & POLLNVAL) != 0)) {
            return false;
        }
        if (media_index >= 0 && !service->media_bound) {
            if (meiso_k230d_composition_edge_service_private_pending_media_candidate_status(
                    service->media_fd,
                    descriptors[media_index].revents
                ) == PENDING_MEDIA_CANDIDATE_CLOSED) {
                meiso_k230d_composition_edge_service_private_close_fd(
                    &service->media_fd
                );
            } else if (!media_candidate_bind_step(service)) {
                return false;
            }
        } else if (media_index >= 0 &&
                   (descriptors[media_index].revents & POLLNVAL) != 0) {
            return false;
        }
        ready.control_readable = control_index >= 0 &&
            (descriptors[control_index].revents &
             (POLLIN | POLLHUP | POLLERR)) != 0;
        ready.control_writable = control_index >= 0 &&
            (descriptors[control_index].revents & POLLOUT) != 0;
        ready.media_readable = media_index >= 0 && service->media_bound &&
            (descriptors[media_index].revents &
             (POLLIN | POLLHUP | POLLERR)) != 0;
        ready.media_writable = media_index >= 0 && service->media_bound &&
            (descriptors[media_index].revents & POLLOUT) != 0;
        ready.camera_poll_allowed = interest.camera_poll_useful;
        ready.now = (MeisoTimePoint){service->edge_clock_epoch, now_ns};
        memset(&result, 0, sizeof(result));
        pump_status = meiso_k230d_composition_edge_runtime_pump(
            service->edge_runtime, &ready, &result
        );
        if (pump_status ==
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_TERMINAL) {
            return false;
        }
        if (pump_status ==
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INVALID_ARGUMENT ||
            pump_status ==
                MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE) {
            return false;
        }
        if (!service->thermal.active) {
            MeisoK230dCompositionEdgeRuntimeIoInterest current_interest;
            if (!meiso_k230d_composition_edge_runtime_readiness(
                    service->edge_runtime, &current_interest
                ) || (current_interest.thermal_request_ready &&
                       !meiso_k230d_composition_edge_service_private_start_thermal_worker(
                           service
                       ))) {
                return false;
            }
        }
    }
    return true;
}
