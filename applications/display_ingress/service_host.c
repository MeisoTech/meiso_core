/* PRODUCT: Display Ingress runtime; reference runners reuse the common service. */
#include "service_private.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

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

static bool wait_event(
    Service *service,
    MeisoRuntimeEvent *out_event,
    const char *stage
)
{
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    while (!service_deadline_expired(deadline)) {
        const ServiceControlStep step = service_control_step(
            service, true, SERVICE_POLL_SLICE_MS, out_event
        );
        if (step == SERVICE_CONTROL_EVENT) {
            return true;
        }
        if (step == SERVICE_CONTROL_FAILED ||
            step == SERVICE_CONTROL_PEER_CLOSED) {
            return service_fail(service, "control", stage);
        }
    }
    return service_fail(service, "control", stage);
}

static bool accept_catalog(Service *service)
{
    MeisoRuntimeEvent event;

    if (!wait_event(service, &event, "catalog") ||
        event.type != MEISO_RUNTIME_EVENT_CATALOG_OFFERED ||
        event.catalog_generation != SERVICE_CATALOG_GENERATION ||
        !service_validate_staged_catalog(service) ||
        meiso_runtime_accept_catalog(service->runtime) != MEISO_RUNTIME_OK) {
        return service_fail(service, "control", "catalog");
    }
    service->catalog_active = true;
    service->catalog_generation = event.catalog_generation;
    return service_flush_control(service);
}

static bool publish_desired(Service *service)
{
    const MeisoDisplayIngressDesired desired = {
        service_frame_width(service), service_frame_height(service), 30000U
    };
    uint8_t runtime_wire[
        MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView runtime_payload;
    MeisoStateOffer offer;
    size_t profile_size = 0U;

    if (meiso_display_ingress_desired_encode(
            &desired,
            (MeisoMutableBuffer){
                service->applied_desired_profile,
                sizeof(service->applied_desired_profile)
            },
            &profile_size
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        profile_size != sizeof(service->applied_desired_profile)) {
        return service_fail(service, "application", "state");
    }
    service->applied_desired.enabled = true;
    service->applied_desired.profile = (MeisoByteView){
        service->applied_desired_profile, profile_size
    };
    if (!meiso_runtime_desired_state_encode(
            &service->applied_desired,
            (MeisoMutableBuffer){runtime_wire, sizeof(runtime_wire)},
            &runtime_payload
        )) {
        return service_fail(service, "application", "state");
    }
    offer = (MeisoStateOffer){
        {SERVICE_DESIRED_DELIVERY_GENERATION},
        {service->host_owner, SERVICE_STATE_VERSION, runtime_payload}
    };
    if (meiso_runtime_send_state_offer(
            service->runtime,
            service_route_key(
                service, MEISO_DISPLAY_INGRESS_ROUTE_DESIRED
            ),
            &offer
        ) != MEISO_RUNTIME_OK) {
        return service_fail(service, "control", "state");
    }
    service->applied_desired_version = offer.state.version;
    return service_flush_control(service);
}

static bool receive_running_actual(Service *service)
{
    const MeisoRuntimeRouteKey expected_route = service_route_key(
        service, MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL
    );
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    while (!service_deadline_expired(deadline)) {
        MeisoRuntimeActualState actual;
        MeisoDisplayIngressActualStateView view;
        MeisoRuntimeEvent event;
        MeisoStateReceiveStatus receive_status;

        if (!wait_event(service, &event, "state")) {
            return false;
        }
        if (event.type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
            if (!service_consume_optional_accept(service, &event)) {
                return false;
            }
            continue;
        }
        if (event.type != MEISO_RUNTIME_EVENT_STATE_OFFER) {
            return service_fail(service, "control", "state");
        }
        if (!route_equal(event.route, expected_route) ||
            !meiso_owner_scope_equal(
                event.state_offer.state.owner, service->edge_owner
            ) || event.state_offer.state.version != SERVICE_STATE_VERSION ||
            event.state_offer.generation.value !=
                SERVICE_ACTUAL_DELIVERY_GENERATION ||
            !meiso_runtime_actual_state_decode(
                event.state_offer.state.payload, &actual
            ) || meiso_display_ingress_validate_actual_state(
                &service->capability,
                &service->applied_desired,
                &actual,
                &view
            ) != MEISO_DISPLAY_INGRESS_PROFILE_OK || !view.data_active) {
            if (meiso_runtime_commit_state_receive(
                    service->runtime,
                    event.route,
                    &event.state_offer,
                    MEISO_STATE_RECEIVE_INVALID
                ) != MEISO_RUNTIME_OK) {
                return service_fail(service, "control", "state");
            }
            return service_fail(service, "application", "state");
        }
        receive_status = meiso_state_replica_receive(
            service->state_replica,
            event.state_offer.state.owner,
            event.state_offer.state.version,
            event.state_offer.state.payload
        );
        if (meiso_runtime_commit_state_receive(
                service->runtime,
                event.route,
                &event.state_offer,
                receive_status
            ) != MEISO_RUNTIME_OK ||
            receive_status != MEISO_STATE_RECEIVE_NEW ||
            meiso_display_ingress_media_binding_from_runtime(
                service->runtime,
                &service->capability,
                &service->applied_desired,
                &service->binding
            ) != MEISO_DISPLAY_INGRESS_PROFILE_OK) {
            return service_fail(service, "application", "binding");
        }
        service->binding_ready = true;
        service->actual_version = event.state_offer.state.version;
        service->production_epoch = service->binding.runtime.production_epoch;
        service->media_stream_epoch = service->binding.media_stream_epoch;
        if (!service_arm_media(service)) {
            meiso_posix_tcp_abort(service->tcp, EPROTO);
            return false;
        }
        return true;
    }
    return service_fail(service, "control", "state");
}

bool service_run_host(Service *service)
{
    return accept_catalog(service) && publish_desired(service) &&
        receive_running_actual(service);
}

#ifndef MEISO_DISPLAY_INGRESS_VIDEO_SERVICE
static bool progress_host_media(Service *service)
{
    const MeisoDisplayIngressMediaPosixUdpPrepareStatus prepare =
        meiso_display_ingress_media_posix_udp_prepare_outbound(service->udp);
    struct pollfd descriptor;
    int poll_status;

    if (prepare != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_READY &&
        prepare != MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_PREPARE_IDLE) {
        return service_fail(service, "media", "transfer");
    }
    if (!meiso_display_ingress_media_posix_udp_wants_write(service->udp)) {
        return true;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = service->media_fd;
    descriptor.events = POLLOUT;
    poll_status = poll(&descriptor, (nfds_t)1U, 0);
    if (poll_status < 0) {
        return errno == EINTR ||
            service_fail(service, "media", "transfer");
    }
    if (poll_status == 0) {
        return true;
    }
    if ((descriptor.revents & POLLOUT) != 0) {
        const MeisoDisplayIngressMediaPosixUdpWriteStatus write_status =
            meiso_display_ingress_media_posix_udp_on_writable(service->udp);

        if (write_status !=
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_PACKET_SENT &&
            write_status !=
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_WOULD_BLOCK &&
            write_status !=
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_WRITE_INTERRUPTED) {
            return service_fail(service, "media", "transfer");
        }
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return service_fail(service, "media", "transfer");
    }
    return true;
}

static bool receive_host_event(
    Service *service,
    const MeisoRuntimeEvent *event
)
{
    MeisoDisplayIngressPresentationEvidence evidence;

    if (event->type == MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
        return service_consume_optional_accept(service, event);
    }
    if (event->type != MEISO_RUNTIME_EVENT_STREAM_SAMPLE ||
        !route_equal(
            event->route,
            service_route_key(
                service,
                MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE
            )
        ) || event->production_epoch != service->production_epoch ||
        !meiso_owner_scope_equal(
            event->stream_sample.key.producer, service->edge_owner
        ) || event->stream_sample.key.epoch !=
            SERVICE_EVIDENCE_STREAM_EPOCH ||
        event->stream_sample.key.sequence !=
            SERVICE_EVIDENCE_STREAM_SEQUENCE ||
        !meiso_time_point_valid(event->stream_sample.source_time) ||
        event->stream_sample.source_time.clock_epoch !=
            SERVICE_EVIDENCE_CLOCK_EPOCH ||
        event->stream_sample.sender_deadline.present ||
        event->stream_sample.dependency.size != 0U ||
        meiso_display_ingress_evidence_decode(
            event->stream_sample.payload, &evidence
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_display_ingress_evidence_supported(
            &service->capability, &evidence, event->production_epoch
        ) ||
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
        evidence.evidence_class != MEISO_DISPLAY_INGRESS_EVIDENCE_DRM_KMS_PAGE_FLIP ||
#else
        evidence.evidence_class != MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED ||
        evidence.connector_kind != MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED ||
        evidence.output_width != service_frame_width(service) ||
        evidence.output_height != service_frame_height(service) ||
#endif
        evidence.media_stream_epoch != service->media_stream_epoch ||
        evidence.displayed_frame_sequence != service->frame_sequence ||
        memcmp(
            evidence.input_rgba_sha256,
            service->input_sha256,
            sizeof(service->input_sha256)
        ) != 0) {
        return service_fail(service, "application", "evidence");
    }
    memmove(
        service->evidence_sha256,
        evidence.input_rgba_sha256,
        sizeof(service->evidence_sha256)
    );
    service->evidence_complete = true;
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    service->presentation = evidence;
    service->presentation_time = event->stream_sample.source_time;
#endif
    return true;
}

bool service_run_host_media(Service *service)
{
    MeisoDisplayIngressMediaFrame frame;
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    if (service == NULL || !service->frame_input_complete ||
        service->frame_fd != -1 ||
        service->frame_input_width != service_frame_width(service) ||
        service->frame_input_height != service_frame_height(service) ||
        service->frame_row_pitch_bytes != service_frame_width(service) * 4U ||
        service->frame_rgba_bytes != SERVICE_FRAME_BYTES) {
        return service_fail(service, "application", "input");
    }
    memset(&frame, 0, sizeof(frame));
    frame.frame_sequence = service->frame_sequence;
    frame.source_time = service->frame_source_time;
    frame.sender_deadline = service->frame_sender_deadline;
    frame.producer_dropped_frames =
        service->frame_producer_dropped_frames;
    frame.rgba_frame = (MeisoByteView){
        service->frame_bytes, sizeof(service->frame_bytes)
    };
    if (!meiso_time_point_valid(frame.source_time) ||
        frame.sender_deadline.present ||
        meiso_display_ingress_media_transport_submit(
            service->media, &frame, NULL
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return service_fail(service, "media", "transfer");
    }
    service->frame_complete = true;

    while (!service->evidence_complete &&
           !service_deadline_expired(deadline)) {
        MeisoRuntimeEvent event;
        const ServiceControlStep control_step =
            progress_host_media(service) ? service_control_step(
                service, true, SERVICE_POLL_SLICE_MS, &event
            ) : SERVICE_CONTROL_FAILED;

        if (control_step == SERVICE_CONTROL_FAILED ||
            control_step == SERVICE_CONTROL_PEER_CLOSED) {
            return service_fail(service, "control", "transfer");
        }
        if (control_step == SERVICE_CONTROL_EVENT &&
            !receive_host_event(service, &event)) {
            return false;
        }
    }
    if (!service->evidence_complete) {
        return service_fail(service, "application", "evidence");
    }
    while (!service_session_tx_idle(service) &&
           !service_deadline_expired(deadline)) {
        MeisoRuntimeEvent event;
        const ServiceControlStep step = service_control_step(
            service, true, SERVICE_POLL_SLICE_MS, &event
        );

        if (step != SERVICE_CONTROL_IDLE) {
            return service_fail(service, "control", "shutdown");
        }
    }
    if (!service_session_tx_idle(service) ||
        !service_terminate_media(
            service, MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE
        )) {
        return false;
    }
    return service_finish_local_close(service);
}

#endif
