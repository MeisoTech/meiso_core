#include "display_ingress_simulated_service_private.h"

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

static bool offer_catalog(Service *service)
{
    MeisoRuntimeEvent event;

    if (meiso_runtime_v4_offer_catalog(
            service->runtime,
            SERVICE_CATALOG_GENERATION,
            service->endpoints,
            service_endpoint_count(service),
            service->routes,
            service_route_count(service)
        ) != MEISO_RUNTIME_OK || !service_flush_control(service) ||
        !wait_event(service, &event, "catalog") ||
        event.type != MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED ||
        event.catalog_generation != SERVICE_CATALOG_GENERATION) {
        return service_fail(service, "control", "catalog");
    }
    service->catalog_active = true;
    service->catalog_generation = event.catalog_generation;
    return true;
}

static bool desired_is_fixture_value(const Service *service, const MeisoDisplayIngressDesired *value)
{
    return value->input_width == service_frame_width(service) &&
        value->input_height == service_frame_height(service) &&
        value->maximum_frame_rate_millihz == 30000U;
}

static bool receive_desired(Service *service)
{
    const MeisoRuntimeRouteKey expected_route = service_route_key(
        service, MEISO_DISPLAY_INGRESS_ROUTE_DESIRED
    );
    MeisoRuntimeDesiredState desired;
    MeisoRuntimeDesiredState candidate;
    MeisoDisplayIngressDesired profile;
    MeisoRuntimeEvent event;
    MeisoStateReceiveStatus receive_status;
    uint8_t candidate_profile[
        MEISO_DISPLAY_INGRESS_PROFILE_DESIRED_WIRE_SIZE
    ];

    if (!wait_event(service, &event, "state") ||
        event.type != MEISO_RUNTIME_EVENT_STATE_OFFER) {
        return service_fail(service, "control", "state");
    }
    if (!route_equal(event.route, expected_route) ||
        !meiso_owner_scope_equal(
            event.state_offer.state.owner, service->host_owner
        ) || event.state_offer.state.version != SERVICE_STATE_VERSION ||
        event.state_offer.generation.value !=
            SERVICE_DESIRED_DELIVERY_GENERATION ||
        !meiso_runtime_desired_state_decode(
            event.state_offer.state.payload, &desired
        ) || !desired.enabled ||
        meiso_display_ingress_desired_decode(desired.profile, &profile) !=
            MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_display_ingress_desired_supported(
            &service->capability, &profile
        ) || !desired_is_fixture_value(service, &profile) ||
        desired.profile.size != sizeof(service->applied_desired_profile)) {
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
    memmove(
        candidate_profile,
        desired.profile.data,
        sizeof(candidate_profile)
    );
    candidate.enabled = true;
    candidate.profile = (MeisoByteView){
        candidate_profile, sizeof(candidate_profile)
    };
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
        ) != MEISO_RUNTIME_OK || receive_status != MEISO_STATE_RECEIVE_NEW) {
        return service_fail(service, "control", "state");
    }
    memmove(
        service->applied_desired_profile,
        candidate.profile.data,
        sizeof(service->applied_desired_profile)
    );
    service->applied_desired.enabled = candidate.enabled;
    service->applied_desired.profile = (MeisoByteView){
        service->applied_desired_profile,
        sizeof(service->applied_desired_profile)
    };
    service->applied_desired_version = event.state_offer.state.version;
    return true;
}

static bool random_channel_nonce(MeisoSessionNonce *out_nonce)
{
    unsigned int attempt;

    for (attempt = 0U; attempt < 4U; ++attempt) {
        if (!service_random_bytes(out_nonce, sizeof(*out_nonce))) {
            return false;
        }
        if (meiso_session_nonce_valid(*out_nonce)) {
            return true;
        }
    }
    return false;
}

static bool queue_running_actual_and_arm(Service *service)
{
    MeisoDisplayIngressActual profile;
    MeisoRuntimeActualState actual;
    MeisoStateOffer offer;
    MeisoDisplayIngressActualStateView view;
    uint8_t profile_wire[MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_WIRE_SIZE];
    uint8_t runtime_wire[
        MEISO_DISPLAY_INGRESS_PROFILE_ACTUAL_STATE_MAX_PAYLOAD_SIZE
    ];
    MeisoByteView runtime_payload;
    size_t profile_size = 0U;

    memset(&profile, 0, sizeof(profile));
    profile.effective_input_width = service_frame_width(service);
    profile.effective_input_height = service_frame_height(service);
    profile.effective_maximum_frame_rate_millihz = 30000U;
    if (!random_channel_nonce(&profile.channel_nonce) ||
        meiso_display_ingress_actual_encode(
            &profile,
            (MeisoMutableBuffer){profile_wire, sizeof(profile_wire)},
            &profile_size
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        profile_size != sizeof(profile_wire)) {
        return service_fail(service, "application", "state");
    }
    memset(&actual, 0, sizeof(actual));
    actual.observed_desired = (MeisoRuntimeDesiredReference){
        true, service->host_owner, SERVICE_STATE_VERSION
    };
    actual.applied_desired = actual.observed_desired;
    actual.apply_result = MEISO_RUNTIME_APPLY_APPLIED;
    actual.availability = MEISO_RUNTIME_AVAILABILITY_PRESENT;
    actual.effective_enabled = true;
    actual.production_status = MEISO_RUNTIME_PRODUCTION_RUNNING;
    actual.reason = MEISO_RUNTIME_PRODUCTION_REASON_NONE;
    actual.production_epoch = SERVICE_PRODUCTION_EPOCH;
    actual.profile = (MeisoByteView){profile_wire, profile_size};
    if (meiso_display_ingress_validate_actual_state(
            &service->capability,
            &service->applied_desired,
            &actual,
            &view
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK || !view.data_active ||
        !meiso_runtime_actual_state_encode(
            &actual,
            (MeisoMutableBuffer){runtime_wire, sizeof(runtime_wire)},
            &runtime_payload
        )) {
        return service_fail(service, "application", "state");
    }
    offer = (MeisoStateOffer){
        {SERVICE_ACTUAL_DELIVERY_GENERATION},
        {service->edge_owner, SERVICE_STATE_VERSION, runtime_payload}
    };
    if (meiso_runtime_send_state_offer(
            service->runtime,
            service_route_key(service, MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL),
            &offer
        ) != MEISO_RUNTIME_OK) {
        return service_fail(service, "control", "state");
    }
    service->running_actual_queued = true;
    service->actual_version = offer.state.version;
    if (meiso_display_ingress_media_binding_from_runtime(
            service->runtime,
            &service->capability,
            &service->applied_desired,
            &service->binding
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK) {
        meiso_posix_tcp_abort(service->tcp, EPROTO);
        return service_fail(service, "media", "binding");
    }
    service->binding_ready = true;
    service->production_epoch = service->binding.runtime.production_epoch;
    service->media_stream_epoch = service->binding.media_stream_epoch;
    if (!service_arm_media(service)) {
        meiso_posix_tcp_abort(service->tcp, EPROTO);
        return false;
    }
    if (!service_flush_control(service)) {
        return false;
    }
    return true;
}

bool service_run_edge(Service *service)
{
    return offer_catalog(service) && receive_desired(service) &&
        queue_running_actual_and_arm(service);
}

#ifndef MEISO_DISPLAY_INGRESS_VIDEO_SERVICE
static bool consume_latest(Service *service)
{
    MeisoDisplayIngressMediaLatestView latest;
    uint8_t digest[MEISO_SHA256_DIGEST_SIZE];

    if (meiso_display_ingress_media_transport_latest(
            service->media, &latest
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK ||
        latest.frame.frame_sequence == 0U ||
        !meiso_time_point_valid(latest.frame.source_time) ||
        latest.frame.sender_deadline.present ||
        latest.frame.rgba_frame.size != SERVICE_FRAME_BYTES ||
        !meiso_display_ingress_host_fixture_input_sha256(
            latest.frame.rgba_frame, digest
        )) {
        return service_fail(service, "media", "transfer");
    }
    service->frame_input_width = service_frame_width(service);
    service->frame_input_height = service_frame_height(service);
    service->frame_row_pitch_bytes = service_frame_width(service) * 4U;
    service->frame_rgba_bytes = SERVICE_FRAME_BYTES;
    service->frame_sequence = latest.frame.frame_sequence;
    service->frame_source_time = latest.frame.source_time;
    service->frame_sender_deadline = latest.frame.sender_deadline;
    service->frame_producer_dropped_frames =
        latest.frame.producer_dropped_frames;
    memmove(service->received_sha256, digest, sizeof(digest));
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    if (!service_drm_present(service, latest.frame.rgba_frame)) {
        return false;
    }
#endif
    if (
        meiso_display_ingress_media_transport_release_latest(
            service->media, &latest.ref
        ) != MEISO_DISPLAY_INGRESS_MEDIA_TRANSPORT_OK) {
        return service_fail(service, "media", "transfer");
    }
    service->frame_complete = true;
    return true;
}

static bool progress_edge_media(Service *service)
{
    struct pollfd descriptor;
    int poll_status;

    if (!meiso_display_ingress_media_posix_udp_wants_read(service->udp)) {
        return service_fail(service, "media", "transfer");
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = service->media_fd;
    descriptor.events = POLLIN;
    poll_status = poll(&descriptor, (nfds_t)1U, 0);
    if (poll_status < 0) {
        return errno == EINTR ||
            service_fail(service, "media", "transfer");
    }
    if (poll_status == 0) {
        return true;
    }
    if ((descriptor.revents & POLLIN) != 0) {
        const MeisoDisplayIngressMediaPosixUdpReadStatus read_status =
            meiso_display_ingress_media_posix_udp_on_readable(
                service->udp, NULL
            );

        if (read_status ==
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAME_PUBLISHED) {
            return !service->frame_complete && consume_latest(service);
        }
        if (read_status !=
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_PACKET_ACCEPTED &&
            read_status !=
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_FRAGMENT_ACCEPTED &&
            read_status !=
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_WOULD_BLOCK &&
            read_status !=
                MEISO_DISPLAY_INGRESS_MEDIA_POSIX_UDP_READ_INTERRUPTED) {
            return service_fail(service, "media", "transfer");
        }
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return service_fail(service, "media", "transfer");
    }
    return true;
}

static bool receive_edge_event(
    Service *service,
    const MeisoRuntimeEvent *event
)
{
    return service_consume_optional_accept(service, event);
}

static bool queue_evidence_if_idle(Service *service)
{
    MeisoDisplayIngressPresentationEvidence evidence;
    MeisoStreamSample sample;
    MeisoRuntimeStatus status;
    uint8_t payload[MEISO_DISPLAY_INGRESS_PROFILE_EVIDENCE_WIRE_SIZE];
    size_t payload_size = 0U;

    if (!service->frame_complete || service->evidence_queued ||
        !service_session_tx_idle(service)) {
        return true;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.evidence_class = MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED;
    evidence.connector_kind = MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED;
    evidence.output_width = service_frame_width(service);
    evidence.output_height = service_frame_height(service);
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    evidence = service->presentation;
#endif
    evidence.media_stream_epoch = service->media_stream_epoch;
    evidence.displayed_frame_sequence = service->frame_sequence;
    memmove(
        evidence.input_rgba_sha256,
        service->received_sha256,
        sizeof(evidence.input_rgba_sha256)
    );
    if (!meiso_display_ingress_evidence_supported(
            &service->capability, &evidence, service->production_epoch
        ) || meiso_display_ingress_evidence_encode(
            &evidence,
            (MeisoMutableBuffer){payload, sizeof(payload)},
            &payload_size
        ) != MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        payload_size != sizeof(payload)) {
        return service_fail(service, "application", "evidence");
    }
    memset(&sample, 0, sizeof(sample));
    sample.key = (MeisoStreamKey){
        service->edge_owner,
        SERVICE_EVIDENCE_STREAM_EPOCH,
        SERVICE_EVIDENCE_STREAM_SEQUENCE
    };
    sample.source_time = (MeisoTimePoint){
        SERVICE_EVIDENCE_CLOCK_EPOCH, service_now_ns()
    };
#ifdef MEISO_DISPLAY_INGRESS_DRM_SERVICE
    sample.source_time = service->presentation_time;
#endif
    sample.payload = (MeisoByteView){payload, payload_size};
    if (!meiso_time_point_valid(sample.source_time)) {
        return service_fail(service, "application", "evidence");
    }
    status = meiso_runtime_send_stream_sample(
        service->runtime,
        service_route_key(
            service,
            MEISO_DISPLAY_INGRESS_ROUTE_PRESENTATION_EVIDENCE
        ),
        service->production_epoch,
        &sample
    );
    if (status == MEISO_RUNTIME_TX_BUSY) {
        return true;
    }
    if (status != MEISO_RUNTIME_OK) {
        return service_fail(service, "control", "evidence");
    }
    memmove(
        service->evidence_sha256,
        evidence.input_rgba_sha256,
        sizeof(service->evidence_sha256)
    );
    service->evidence_queued = true;
    return true;
}

bool service_run_edge_media(Service *service)
{
    const uint64_t deadline = service_deadline_after_ms(SERVICE_TIMEOUT_MS);

    while (!service->evidence_complete &&
           !service_deadline_expired(deadline)) {
        MeisoRuntimeEvent event;
        const ServiceControlStep control_step =
            progress_edge_media(service) ? service_control_step(
                service, true, SERVICE_POLL_SLICE_MS, &event
            ) : SERVICE_CONTROL_FAILED;

        if (control_step == SERVICE_CONTROL_FAILED ||
            control_step == SERVICE_CONTROL_PEER_CLOSED) {
            return service_fail(service, "control", "transfer");
        }
        if (control_step == SERVICE_CONTROL_EVENT &&
            !receive_edge_event(service, &event)) {
            return false;
        }
        if (!queue_evidence_if_idle(service)) {
            return false;
        }
        if (service->evidence_queued && service_session_tx_idle(service)) {
            service->evidence_complete = true;
        }
    }
    if (!service->frame_complete || !service->evidence_complete ||
        !service_terminate_media(
            service, MEISO_DISPLAY_INGRESS_MEDIA_TERMINAL_LOCAL_CLOSE
        ) || !service_finish_peer_close(service)) {
        return service_fail(service, "application", "evidence");
    }
    return true;
}

#endif
