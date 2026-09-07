#define _POSIX_C_SOURCE 200809L

#include "display_ingress_simulated_service_private.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

static bool write_status_ok(MeisoPosixTcpWriteStatus status)
{
    return status == MEISO_POSIX_TCP_WRITE_PROGRESS ||
        status == MEISO_POSIX_TCP_WRITE_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_WRITE_INTERRUPTED ||
        status == MEISO_POSIX_TCP_WRITE_FRAME_SENT ||
        status == MEISO_POSIX_TCP_WRITE_IDLE;
}

static bool control_write_allowed(Service *service)
{
    return !service->running_actual_queued ||
        service->udp_armed_before_actual_write ||
        service_fail(service, "media", "binding");
}

static bool read_status_is_handshake_progress(MeisoPosixTcpReadStatus status)
{
    return status == MEISO_POSIX_TCP_READ_PROGRESS ||
        status == MEISO_POSIX_TCP_READ_WOULD_BLOCK ||
        status == MEISO_POSIX_TCP_READ_INTERRUPTED ||
        status == MEISO_POSIX_TCP_READ_SESSION_PROGRESS;
}

bool service_session_tx_idle(const Service *service)
{
    MeisoSessionSnapshot snapshot;

    return service != NULL && service->session != NULL &&
        meiso_session_snapshot(service->session, &snapshot) &&
        !snapshot.outbound_pending;
}

bool service_wait_session_ready(Service *service)
{
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    while (!service_deadline_expired(deadline)) {
        MeisoSessionSnapshot snapshot;
        struct pollfd descriptor;
        int poll_status;

        if (!meiso_session_snapshot(service->session, &snapshot)) {
            return service_fail(service, "control", "session");
        }
        if (snapshot.state == MEISO_SESSION_STATE_READY &&
            snapshot.failure == MEISO_SESSION_FAILURE_NONE &&
            snapshot.remote_identity_known && snapshot.session_id_known) {
            return true;
        }
        if (snapshot.state == MEISO_SESSION_STATE_FAILED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            return service_fail(service, "control", "session");
        }
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.fd = service->control_fd;
        if (meiso_posix_tcp_wants_read(service->tcp)) {
            descriptor.events = (short)(descriptor.events | POLLIN);
        }
        if (meiso_posix_tcp_wants_write(service->tcp)) {
            descriptor.events = (short)(descriptor.events | POLLOUT);
        }
        if (descriptor.events == 0) {
            return service_fail(service, "control", "session");
        }
        poll_status = poll(&descriptor, (nfds_t)1U, SERVICE_POLL_SLICE_MS);
        if (poll_status < 0) {
            if (errno == EINTR) {
                continue;
            }
            return service_fail(service, "control", "session");
        }
        if (poll_status == 0) {
            continue;
        }
        if ((descriptor.revents & POLLOUT) != 0 &&
            (!control_write_allowed(service) || !write_status_ok(
                meiso_posix_tcp_on_writable(service->tcp)
            ))) {
            return service_fail(service, "control", "session");
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            MeisoSessionRuntimeMessage unexpected;
            const MeisoPosixTcpReadStatus read_status =
                meiso_posix_tcp_on_readable(service->tcp, &unexpected);
            if (!read_status_is_handshake_progress(read_status)) {
                return service_fail(service, "control", "session");
            }
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            return service_fail(service, "control", "session");
        }
    }
    return service_fail(service, "control", "session");
}

ServiceControlStep service_control_step(
    Service *service,
    bool allow_write,
    int timeout_ms,
    MeisoRuntimeEvent *out_event
)
{
    struct pollfd descriptor;
    int poll_status;

    if (service == NULL || service->tcp == NULL || service->runtime == NULL ||
        out_event == NULL || timeout_ms < 0) {
        (void)service_fail(service, "control", "transfer");
        return SERVICE_CONTROL_FAILED;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = service->control_fd;
    if (meiso_posix_tcp_wants_read(service->tcp)) {
        descriptor.events = (short)(descriptor.events | POLLIN);
    }
    if (allow_write && meiso_posix_tcp_wants_write(service->tcp)) {
        descriptor.events = (short)(descriptor.events | POLLOUT);
    }
    poll_status = poll(&descriptor, (nfds_t)1U, timeout_ms);
    if (poll_status < 0) {
        if (errno == EINTR) {
            return SERVICE_CONTROL_IDLE;
        }
        (void)service_fail(service, "control", "transfer");
        return SERVICE_CONTROL_FAILED;
    }
    if (poll_status == 0) {
        return SERVICE_CONTROL_IDLE;
    }
    if ((descriptor.revents & POLLOUT) != 0 &&
        (!control_write_allowed(service) || !write_status_ok(
            meiso_posix_tcp_on_writable(service->tcp)
        ))) {
        (void)service_fail(service, "control", "transfer");
        return SERVICE_CONTROL_FAILED;
    }
    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
        MeisoSessionRuntimeMessage message;
        const MeisoPosixTcpReadStatus read_status =
            meiso_posix_tcp_on_readable(service->tcp, &message);

        if (read_status == MEISO_POSIX_TCP_READ_RUNTIME) {
            if (meiso_runtime_receive(
                    service->runtime, &message, out_event
                ) != MEISO_RUNTIME_OK) {
                (void)service_fail(service, "control", "transfer");
                return SERVICE_CONTROL_FAILED;
            }
            return SERVICE_CONTROL_EVENT;
        }
        if (read_status == MEISO_POSIX_TCP_READ_PEER_CLOSED) {
            return SERVICE_CONTROL_PEER_CLOSED;
        }
        if (!read_status_is_handshake_progress(read_status)) {
            (void)service_fail(service, "control", "transfer");
            return SERVICE_CONTROL_FAILED;
        }
    }
    if ((descriptor.revents & POLLNVAL) != 0) {
        (void)service_fail(service, "control", "transfer");
        return SERVICE_CONTROL_FAILED;
    }
    return SERVICE_CONTROL_IDLE;
}

bool service_flush_control(Service *service)
{
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    while (!service_session_tx_idle(service) &&
           !service_deadline_expired(deadline)) {
        struct pollfd descriptor = {service->control_fd, POLLOUT, 0};
        const int poll_status = poll(
            &descriptor, (nfds_t)1U, SERVICE_POLL_SLICE_MS
        );

        if (poll_status < 0) {
            if (errno == EINTR) {
                continue;
            }
            return service_fail(service, "control", "transfer");
        }
        if (poll_status == 0) {
            continue;
        }
        if ((descriptor.revents & POLLOUT) != 0 &&
            (!control_write_allowed(service) || !write_status_ok(
                meiso_posix_tcp_on_writable(service->tcp)
            ))) {
            return service_fail(service, "control", "transfer");
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
            (descriptor.revents & POLLOUT) == 0) {
            return service_fail(service, "control", "transfer");
        }
    }
    return service_session_tx_idle(service) ||
        service_fail(service, "control", "transfer");
}

MeisoRuntimeRouteKey service_route_key(
    const Service *service,
    MeisoDisplayIngressProfileRoute route
)
{
    MeisoRuntimeRouteKey key;
    const MeisoRuntimeRouteDescriptor *descriptor;

    memset(&key, 0, sizeof(key));
    if (service == NULL || route >= service_route_count(service)) {
        return key;
    }
    descriptor = &service->routes[route].route;
    key.catalog_generation = SERVICE_CATALOG_GENERATION;
    key.endpoint_id = descriptor->endpoint_id;
    key.endpoint_incarnation = descriptor->endpoint_incarnation;
    key.resource_id = descriptor->resource.id;
    key.facet = descriptor->facet;
    return key;
}

static bool capability_equal(
    const MeisoDisplayIngressCapability *lhs,
    const MeisoDisplayIngressCapability *rhs
)
{
    return lhs->max_input_width == rhs->max_input_width &&
        lhs->max_input_height == rhs->max_input_height &&
        lhs->max_frame_rate_millihz == rhs->max_frame_rate_millihz &&
        lhs->max_frame_bytes == rhs->max_frame_bytes &&
        lhs->max_fragment_payload_bytes == rhs->max_fragment_payload_bytes &&
        lhs->evidence_classes == rhs->evidence_classes &&
        lhs->deadline_policy == rhs->deadline_policy &&
        lhs->frame_format == rhs->frame_format;
}

bool service_validate_staged_catalog(Service *service)
{
    MeisoRuntimeEndpointDescriptor endpoint;
    MeisoRuntimeV4RouteDescriptor routes[
        MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT
    ];
    MeisoDisplayIngressCapability decoded;
    size_t index;

    if (meiso_runtime_staged_endpoint_at(
            service->runtime, 0U, &endpoint
        ) != MEISO_RUNTIME_OK) {
        return service_fail(service, "control", "catalog");
    }
    for (index = 0U;
         index < MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT;
         ++index) {
        if (meiso_runtime_v4_staged_route_at(
                service->runtime, index, &routes[index]
            ) != MEISO_RUNTIME_OK) {
            return service_fail(service, "control", "catalog");
        }
    }
    if (meiso_display_ingress_validate_catalog(
            &endpoint,
            MEISO_DISPLAY_INGRESS_PROFILE_ENDPOINT_COUNT,
            routes,
            MEISO_DISPLAY_INGRESS_PROFILE_ROUTE_COUNT,
            &decoded
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !capability_equal(&decoded, &service->capability)) {
        return service_fail(service, "control", "catalog");
    }
    return true;
}
