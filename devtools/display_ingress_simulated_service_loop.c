#include "display_ingress_simulated_service_private.h"

#include <string.h>

bool service_terminate_media(
    Service *service,
    MeisoDisplayIngressMediaTerminalReason reason
)
{
    MeisoDisplayIngressMediaTransportSnapshot before;
    MeisoDisplayIngressMediaTransportSnapshot after;
    MeisoDisplayIngressMediaPosixUdpSnapshot udp_after;

    if (service == NULL || service->media == NULL || service->udp == NULL ||
        meiso_display_ingress_media_transport_snapshot(
            service->media, &before
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        before.phase != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_ACTIVE ||
        before.outbound_packet_pending || before.outbound_preemption_pending ||
        before.send_in_flight || before.send_latest ||
        before.receive_reassembly || before.receive_latest ||
        meiso_display_ingress_media_posix_udp_terminate(
            service->udp, reason, 0
        ) != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_TERMINATE_OK ||
        !meiso_display_ingress_media_posix_udp_snapshot(
            service->udp, &udp_after
        ) || !udp_after.terminal || udp_after.terminal_reason != reason ||
        udp_after.last_error != 0 || udp_after.tx_packet_borrowed ||
        meiso_display_ingress_media_transport_snapshot(
            service->media, &after
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        after.phase != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_PHASE_TERMINAL ||
        after.terminal_reason != reason || after.outbound_packet_pending ||
        after.outbound_preemption_pending || after.send_in_flight ||
        after.send_latest || after.receive_reassembly ||
        after.receive_latest) {
        return service_fail(service, "media", "shutdown");
    }
    service->media_terminated = true;
    return true;
}

static bool route_equal(
    MeisoRuntimeRouteKey lhs,
    MeisoRuntimeRouteKey rhs
)
{
    return lhs.catalog_generation == rhs.catalog_generation &&
        meiso_endpoint_id_equal(lhs.endpoint_id, rhs.endpoint_id) &&
        lhs.endpoint_incarnation == rhs.endpoint_incarnation &&
        meiso_resource_id_equal(lhs.resource_id, rhs.resource_id) &&
        lhs.facet == rhs.facet;
}

bool service_consume_optional_accept(
    Service *service,
    const MeisoRuntimeEvent *event
)
{
    MeisoRuntimeRouteKey expected_route;
    MeisoOwnerScope expected_owner;
    uint64_t expected_generation;
    uint64_t expected_version;

    if (service == NULL || event == NULL ||
        event->type != MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
        return service_fail(service, "control", "state");
    }
    if (service->role == MEISO_SESSION_ROLE_HOST) {
        expected_route = service_route_key(
            service, MEISO_DISPLAY_INGRESS_ROUTE_DESIRED
        );
        expected_owner = service->host_owner;
        expected_generation = SERVICE_DESIRED_DELIVERY_GENERATION;
        expected_version = service->applied_desired_version;
    } else {
        expected_route = service_route_key(
            service, MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL
        );
        expected_owner = service->edge_owner;
        expected_generation = SERVICE_ACTUAL_DELIVERY_GENERATION;
        expected_version = service->actual_version;
    }
    if (!route_equal(event->route, expected_route) ||
        event->state_accept_generation.value != expected_generation ||
        !meiso_owner_scope_equal(event->state_accept_owner, expected_owner) ||
        event->state_accept_version != expected_version) {
        return service_fail(service, "control", "state");
    }
    return true;
}

bool service_finish_local_close(Service *service)
{
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);
    MeisoSessionCloseStatus close_status;

    if (service == NULL || !service->media_terminated) {
        return service_fail(service, "control", "shutdown");
    }
    do {
        MeisoRuntimeEvent event;
        const ServiceControlStep step = service_control_step(
            service, true, SERVICE_POLL_SLICE_MS, &event
        );

        if ((step == SERVICE_CONTROL_EVENT &&
             !service_consume_optional_accept(service, &event)) ||
            step == SERVICE_CONTROL_FAILED || step == SERVICE_CONTROL_PEER_CLOSED) {
            return service_fail(service, "control", "shutdown");
        }
    } while (!service_session_tx_idle(service) &&
             !service_deadline_expired(deadline));
    if (!service_session_tx_idle(service)) {
        return service_fail(service, "control", "shutdown");
    }
    close_status = meiso_session_close(
        service->session, MEISO_SESSION_CLOSE_NORMAL
    );
    if (close_status != MEISO_SESSION_CLOSE_QUEUED) {
        return service_fail(service, "control", "shutdown");
    }
    while (!service_deadline_expired(deadline)) {
        MeisoSessionSnapshot snapshot;
        MeisoRuntimeEvent event;
        ServiceControlStep step;

        if (!meiso_session_snapshot(service->session, &snapshot)) {
            return service_fail(service, "control", "shutdown");
        }
        if (snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            if (snapshot.failure != MEISO_SESSION_FAILURE_LOCAL_CLOSED ||
                snapshot.outbound_pending ||
                !snapshot.local_close_reason_known ||
                snapshot.local_close_reason != MEISO_SESSION_CLOSE_NORMAL) {
                return service_fail(service, "control", "shutdown");
            }
            service->close_clean = true;
            return true;
        }
        step = service_control_step(
            service, true, SERVICE_POLL_SLICE_MS, &event
        );
        if ((step == SERVICE_CONTROL_EVENT &&
             !service_consume_optional_accept(service, &event)) ||
            step == SERVICE_CONTROL_FAILED || step == SERVICE_CONTROL_PEER_CLOSED) {
            return service_fail(service, "control", "shutdown");
        }
    }
    return service_fail(service, "control", "shutdown");
}

bool service_finish_peer_close(Service *service)
{
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    if (service == NULL || !service->media_terminated) {
        return service_fail(service, "control", "shutdown");
    }
    while (!service_deadline_expired(deadline)) {
        MeisoSessionSnapshot snapshot;
        MeisoRuntimeEvent event;
        const ServiceControlStep step = service_control_step(
            service, true, SERVICE_POLL_SLICE_MS, &event
        );

        if ((step == SERVICE_CONTROL_EVENT &&
             !service_consume_optional_accept(service, &event)) ||
            step == SERVICE_CONTROL_FAILED) {
            return service_fail(service, "control", "shutdown");
        }
        if (!meiso_session_snapshot(service->session, &snapshot)) {
            return service_fail(service, "control", "shutdown");
        }
        if (step == SERVICE_CONTROL_PEER_CLOSED ||
            snapshot.state == MEISO_SESSION_STATE_CLOSED) {
            if (snapshot.state != MEISO_SESSION_STATE_CLOSED ||
                snapshot.failure != MEISO_SESSION_FAILURE_PEER_CLOSED ||
                snapshot.outbound_pending ||
                !snapshot.peer_close_reason_known ||
                snapshot.peer_close_reason != MEISO_SESSION_CLOSE_NORMAL) {
                return service_fail(service, "control", "shutdown");
            }
            service->close_clean = true;
            return true;
        }
    }
    return service_fail(service, "control", "shutdown");
}

void service_cleanup_failure(Service *service)
{
    MeisoSessionSnapshot snapshot;
    MeisoDisplayIngressMediaTerminalReason reason =
        MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_PROTOCOL_BINDING;

    if (service == NULL) {
        return;
    }
    if (service->failure_plane != NULL &&
        strcmp(service->failure_plane, "control") == 0) {
        reason = MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_CONTROL_SESSION;
    } else if (service->failure_plane != NULL &&
               service->failure_stage != NULL &&
               strcmp(service->failure_plane, "media") == 0 &&
               strcmp(service->failure_stage, "transfer") == 0) {
        reason = MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST;
    }
    if (service->udp != NULL) {
        (void)meiso_display_ingress_media_posix_udp_terminate(
            service->udp, reason, 0
        );
    } else if (service->media != NULL && service->binding_ready) {
        (void)meiso_display_ingress_media_transport_terminate(
            service->media, &service->binding, reason
        );
    }
    if (service->tcp != NULL) {
        meiso_posix_tcp_abort(service->tcp, 0);
    } else if (service->session != NULL &&
               meiso_session_snapshot(service->session, &snapshot) &&
               snapshot.state != MEISO_SESSION_STATE_CLOSED &&
               snapshot.state != MEISO_SESSION_STATE_FAILED) {
        meiso_session_transport_lost(service->session);
    }
}

bool service_run(Service *service)
{
    bool control_ready;
    bool success;

    if (!service_bootstrap(service) || !service_wait_session_ready(service) ||
        !service_initialize_runtime(service)) {
        service_cleanup_failure(service);
        return false;
    }
    control_ready = service->role == MEISO_SESSION_ROLE_HOST ?
        service_run_host(service) : service_run_edge(service);
    if (!control_ready) {
        service_cleanup_failure(service);
        return false;
    }
    success = service->role == MEISO_SESSION_ROLE_HOST ?
        service_run_host_media(service) : service_run_edge_media(service);
    if (!success) {
        service_cleanup_failure(service);
    }
    return success;
}
