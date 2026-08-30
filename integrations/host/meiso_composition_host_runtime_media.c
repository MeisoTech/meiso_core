/* External media scope admission, transport pumping and frame borrows. */
#include "meiso_composition_host_runtime_private.h"

#include <errno.h>
#include <stdalign.h>
#include <string.h>

static bool media_desired_equal(
    const MeisoCompositionMediaDesired *lhs,
    const MeisoCompositionMediaDesired *rhs
)
{
    return lhs->pixel_format == rhs->pixel_format &&
        lhs->codec == rhs->codec && lhs->width == rhs->width &&
        lhs->height == rhs->height &&
        lhs->fps_millihz == rhs->fps_millihz &&
        lhs->bitrate_bps == rhs->bitrate_bps &&
        lhs->gop_frames == rhs->gop_frames &&
        lhs->recovery_flags == rhs->recovery_flags;
}

static bool media_binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        lhs->endpoint_id.high == rhs->endpoint_id.high &&
        lhs->endpoint_id.low == rhs->endpoint_id.low &&
        lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
        meiso_resource_id_equal(lhs->resource_id, rhs->resource_id) &&
        lhs->source == rhs->source &&
        lhs->production_epoch == rhs->production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low;
}

static bool media_desired_channels_converged(
    const MeisoCompositionHostRuntime *runtime
)
{
    static const size_t MEDIA_CHANNELS[
        MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT
    ] = {
        MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_RENDER_MEDIA,
        MEISO_COMPOSITION_HOST_RUNTIME_DESIRED_CAMERA_MEDIA
    };
    size_t media_index;
    for (media_index = 0U;
         media_index < MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT;
         ++media_index) {
        const size_t index = MEDIA_CHANNELS[media_index];
        const MeisoCompositionHostRuntimeDesiredSnapshot *desired =
            &runtime->desired[index].snapshot;
        const MeisoCompositionHostRuntimeActualSnapshot *actual =
            &runtime->actual[index].snapshot;
        if (!desired->requested) {
            if (actual->present) {
                return false;
            }
            continue;
        }
        if (desired->inflight_present || desired->delivery_pending ||
            !desired->delivery_accepted || desired->delivery_failed ||
            !actual->present || !actual->observed_desired.present ||
            !actual->applied_desired.present ||
            actual->observed_desired.owner.role != desired->owner.role ||
            actual->observed_desired.owner.incarnation !=
                desired->owner.incarnation ||
            actual->observed_desired.version != desired->state_version ||
            actual->applied_desired.owner.role != desired->owner.role ||
            actual->applied_desired.owner.incarnation !=
                desired->owner.incarnation ||
            actual->applied_desired.version != desired->state_version) {
            return false;
        }
    }
    return true;
}

bool meiso_composition_host_runtime_private_media_route_config(
    const MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionControlSessionSnapshot *control_session,
    size_t media_index,
    MeisoCompositionMediaRouteConfig *out_route
)
{
    const size_t actual_index = media_index == 0U ?
        MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_RENDER_MEDIA :
        MEISO_COMPOSITION_HOST_RUNTIME_ACTUAL_CAMERA_MEDIA;
    const size_t route_index = media_index == 0U ?
        MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_RENDER_MEDIA_DATA : MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_ROUTE_CAMERA_MEDIA_DATA;
    const MeisoCompositionMediaActual *media = media_index == 0U ?
        &runtime->render_media_actual : &runtime->camera_media_actual;
    const MeisoCompositionMediaCapability *capability = media_index == 0U ?
        &runtime->config.catalog.render_media_capability :
        &runtime->config.catalog.camera_media_capability;
    const MeisoCompositionHostRuntimeActualSnapshot *actual =
        &runtime->actual[actual_index].snapshot;
    const MeisoRuntimeRouteDescriptor *descriptor =
        &runtime->catalog_routes[route_index].route;
    MeisoCompositionMediaRouteConfig route;
    if (out_route == NULL || !actual->present || !actual->profile_decoded ||
        !actual->effective_enabled ||
        actual->availability != MEISO_RUNTIME_AVAILABILITY_PRESENT ||
        actual->production_status != MEISO_RUNTIME_PRODUCTION_RUNNING ||
        actual->production_epoch == 0U || media->media_stream_epoch == 0U ||
        !meiso_session_nonce_valid(media->channel_nonce) ||
        !meiso_composition_media_desired_supported(
            capability, &media->effective
        )) {
        return false;
    }
    memset(&route, 0, sizeof(route));
    route.binding.session_id = control_session->session.session_id;
    route.binding.catalog_generation = control_session->runtime.active_catalog_generation;
    route.binding.endpoint_id = descriptor->endpoint_id;
    route.binding.endpoint_incarnation = descriptor->endpoint_incarnation;
    route.binding.resource_id = descriptor->resource.id;
    route.binding.source = media->source;
    route.binding.production_epoch = actual->production_epoch;
    route.binding.media_stream_epoch = media->media_stream_epoch;
    route.binding.channel_nonce = media->channel_nonce;
    route.capability = *capability;
    route.effective = media->effective;
    *out_route = route;
    return true;
}

static bool build_media_config(
    const MeisoCompositionHostRuntime *runtime,
    MeisoCompositionMediaTransportConfig *out_config
)
{
    MeisoCompositionControlSessionSnapshot control_session;
    MeisoCompositionMediaTransportConfig config;
    size_t index;
    if (out_config == NULL ||
        !meiso_composition_control_session_snapshot(runtime->control_session, &control_session) ||
        control_session.phase != MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY ||
        control_session.runtime.active_catalog_generation != runtime->config.catalog_generation ||
        control_session.session.state != MEISO_SESSION_STATE_READY ||
        !control_session.session.session_id_known) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_HOST;
    config.scope.session_id = control_session.session.session_id;
    config.scope.catalog_generation = runtime->config.catalog_generation;
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        if (meiso_composition_host_runtime_private_media_route_config(runtime, &control_session, index, &config.routes[index])) {
            config.active_route_mask =
                (uint8_t)(config.active_route_mask | bit);
        } else {
            config.routes[index].capability = index == 0U ?
                runtime->config.catalog.render_media_capability :
                runtime->config.catalog.camera_media_capability;
        }
    }
    if (config.active_route_mask == 0U) {
        return false;
    }
    *out_config = config;
    return true;
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_bind_media_fd(
    MeisoCompositionHostRuntime *runtime,
    int connected_fd,
    MeisoCompositionHostRuntimeMediaRouteMask active_routes
)
{
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    MeisoCompositionMediaTransportConfig transport_config;
    MeisoCompositionMediaPosixTcpConfig adapter_config;
    MeisoCompositionHostRuntimePrivateMediaSlot *slot;
    MeisoCompositionHostRuntimePrivateMediaSlot slot_before;
    size_t transport_bytes;
    size_t transport_alignment;
    size_t adapter_bytes;
    uint8_t *slot_bytes;
    MeisoCompositionMediaTransportStatus transport_status;
    MeisoCompositionMediaPosixTcpInitStatus adapter_status;
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        return status;
    }
    if (active_routes == 0U ||
        (active_routes & ~MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ALL) != 0U) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (connected_fd == runtime->config.control_connected_fd) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (!media_desired_channels_converged(runtime)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_NOT_READY;
    }
    if (runtime->active_media != NULL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_BUSY;
    }
    if (runtime->media_slots_consumed >= runtime->config.media_connection_slots) {
        return MEISO_COMPOSITION_HOST_RUNTIME_CAPACITY;
    }
    if (!build_media_config(runtime, &transport_config)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_NOT_READY;
    }
    if (active_routes != transport_config.active_route_mask) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE;
    }
    if (!meiso_composition_host_runtime_private_fd_preflight(connected_fd) || !meiso_composition_host_runtime_private_fd_prepare_send_safety(connected_fd)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (meiso_composition_media_transport_memory_required(
            &transport_config, &transport_bytes, &transport_alignment
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        transport_bytes > runtime->media_transport_bytes ||
        transport_alignment > alignof(max_align_t)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_CAPACITY;
    }
    slot = &runtime->media_slots[runtime->media_slots_consumed];
    slot_before = *slot;
    slot_bytes = runtime->arena_begin + slot->offset;
    transport_status = meiso_composition_media_transport_init(
        &transport_config,
        (MeisoMutableBuffer){slot_bytes, runtime->media_transport_bytes},
        &slot->transport
    );
    if (transport_status != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        memset(slot_bytes, 0, runtime->media_slot_bytes);
        *slot = slot_before;
        return MEISO_COMPOSITION_HOST_RUNTIME_INTERNAL_FAILURE;
    }
    memset(&adapter_config, 0, sizeof(adapter_config));
    adapter_config.connected_fd = connected_fd;
    adapter_config.transport = slot->transport;
    adapter_config.scope = transport_config.scope;
    adapter_config.max_io_bytes_per_step =
        runtime->config.max_media_io_bytes_per_step;
    adapter_bytes = meiso_composition_media_posix_tcp_memory_required(
        &adapter_config
    );
    if (adapter_bytes == 0U ||
        adapter_bytes > MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MEDIA_ADAPTER_RESERVE_BYTES) {
        (void)meiso_composition_media_transport_terminate(
            slot->transport, &transport_config.scope,
            MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST
        );
        memset(slot_bytes, 0, runtime->media_slot_bytes);
        *slot = slot_before;
        return MEISO_COMPOSITION_HOST_RUNTIME_CAPACITY;
    }
    adapter_status = meiso_composition_media_posix_tcp_init(
        slot_bytes + runtime->media_transport_bytes,
        MEISO_COMPOSITION_HOST_RUNTIME_PRIVATE_MEDIA_ADAPTER_RESERVE_BYTES, &adapter_config,
        &slot->adapter
    );
    if (adapter_status != MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK) {
        (void)meiso_composition_media_transport_terminate(
            slot->transport, &transport_config.scope,
            MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST
        );
        memset(slot_bytes, 0, runtime->media_slot_bytes);
        *slot = slot_before;
        return MEISO_COMPOSITION_HOST_RUNTIME_INTERNAL_FAILURE;
    }
    slot->consumed = true;
    slot->fd = connected_fd;
    slot->active_route_mask = active_routes;
    slot->transport_config = transport_config;
    ++runtime->media_slots_consumed;
    runtime->active_media = slot;
    return MEISO_COMPOSITION_HOST_RUNTIME_OK;
}

bool meiso_composition_host_runtime_private_active_media_scope_current(MeisoCompositionHostRuntime *runtime)
{
    MeisoCompositionMediaTransportConfig current;
    size_t index;
    if (runtime->active_media == NULL || !build_media_config(runtime, &current) ||
        current.active_route_mask != runtime->active_media->active_route_mask) {
        return false;
    }
    if (!meiso_session_id_equal(
            current.scope.session_id,
            runtime->active_media->transport_config.scope.session_id
        ) || current.scope.catalog_generation !=
            runtime->active_media->transport_config.scope.catalog_generation) {
        return false;
    }
    for (index = 0U; index < MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        if ((current.active_route_mask & bit) != 0U &&
            (!media_binding_equal(
                &current.routes[index].binding,
                &runtime->active_media->transport_config.routes[index].binding
             ) || !media_desired_equal(
                &current.routes[index].effective,
                &runtime->active_media->transport_config.routes[index].effective
             ))) {
            return false;
        }
    }
    return true;
}

void meiso_composition_host_runtime_private_terminate_active_media(
    MeisoCompositionHostRuntime *runtime,
    MeisoCompositionMediaTerminalReason reason,
    int error_number
)
{
    if (runtime->active_media != NULL) {
        (void)meiso_composition_media_posix_tcp_terminate(
            runtime->active_media->adapter, reason, error_number
        );
        runtime->active_media = NULL;
    }
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_unbind_media_fd(
    MeisoCompositionHostRuntime *runtime,
    int error_number
)
{
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        return status;
    }
    if (runtime->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE;
    }
    meiso_composition_host_runtime_private_terminate_active_media(
        runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE, error_number
    );
    return MEISO_COMPOSITION_HOST_RUNTIME_OK;
}

MeisoCompositionHostRuntimePumpStatus meiso_composition_host_runtime_private_pump_media(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionHostRuntimeIoReady *ready,
    MeisoCompositionHostRuntimePumpResult *out_result
)
{
    MeisoCompositionMediaPosixTcpReadStatus read_status;
    MeisoCompositionMediaPosixTcpWriteStatus write_status;
    MeisoCompositionMediaPosixTcpPrepareStatus prepare_status;
    MeisoCompositionMediaPosixTcpSnapshot before;
    MeisoCompositionMediaTransportSnapshot transport;
    size_t index;
    if (runtime->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE;
    }
    if (!meiso_composition_host_runtime_private_active_media_scope_current(runtime)) {
        meiso_composition_host_runtime_private_terminate_active_media(
            runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, EPROTO
        );
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_BOUNDARY;
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
    }
    if (ready->media_readable) {
        read_status = meiso_composition_media_posix_tcp_on_readable(
            runtime->active_media->adapter
        );
        if (read_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK ||
            read_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED) {
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_WOULD_BLOCK;
        }
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST ||
            read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INVALID_ARGUMENT) {
            meiso_composition_host_runtime_private_terminate_active_media(
                runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
                EPROTO
            );
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_BOUNDARY;
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_PROGRESS;
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_FRAME_PUBLISHED &&
            meiso_composition_media_transport_snapshot(
                runtime->active_media->transport, &transport
            ) == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
            for (index = 0U;
                 index < MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT;
                 ++index) {
                if (transport.routes[index].published_sequence_high_water !=
                    runtime->active_media->published_high_water[index]) {
                    runtime->active_media->published_high_water[index] =
                        transport.routes[index].published_sequence_high_water;
                    out_result->event =
                        MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_FRAME;
                    out_result->media_source_present = true;
                    out_result->media_source = transport.routes[index].source;
                    break;
                }
            }
        }
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
    }
    if (!meiso_composition_media_posix_tcp_snapshot(
            runtime->active_media->adapter, &before
        )) {
        meiso_composition_host_runtime_private_terminate_active_media(
            runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    prepare_status = meiso_composition_media_posix_tcp_prepare_outbound(
        runtime->active_media->adapter
    );
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TRANSPORT_INVALID ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_INVALID_ARGUMENT) {
        meiso_composition_host_runtime_private_terminate_active_media(
            runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
            EPROTO
        );
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY) {
        if (!ready->media_writable) {
            if (!before.tx_packet_borrowed) {
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_PROGRESS;
                return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
            }
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE;
        }
        write_status = meiso_composition_media_posix_tcp_on_writable(
            runtime->active_media->adapter
        );
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED) {
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_WOULD_BLOCK;
        }
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_INVALID ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INVALID_ARGUMENT) {
            meiso_composition_host_runtime_private_terminate_active_media(
                runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST,
                EPROTO
            );
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_BOUNDARY;
            return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_COMPOSITION_HOST_RUNTIME_PUMP_MEDIA_PROGRESS;
        return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_PROGRESS;
    }
    return MEISO_COMPOSITION_HOST_RUNTIME_PUMP_IDLE;
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_media_latest(
    const MeisoCompositionHostRuntime *runtime,
    MeisoCompositionMediaSource source,
    MeisoCompositionMediaLatestView *out_latest
)
{
    MeisoCompositionMediaTransportStatus status;
    if (!meiso_composition_host_runtime_private_valid(runtime) || out_latest == NULL ||
        meiso_composition_host_runtime_private_range_overlap(runtime, out_latest, sizeof(*out_latest)) ||
        (source != MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
         source != MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA)) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->phase == MEISO_COMPOSITION_HOST_RUNTIME_PHASE_TERMINAL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_TERMINAL;
    }
    if (runtime->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_NO_FRAME;
    }
    status = meiso_composition_media_transport_latest(
        runtime->active_media->transport, source, out_latest
    );
    if (status == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return MEISO_COMPOSITION_HOST_RUNTIME_OK;
    }
    return status == MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME ?
        MEISO_COMPOSITION_HOST_RUNTIME_NO_FRAME :
        MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE;
}

MeisoCompositionHostRuntimeStatus meiso_composition_host_runtime_media_release(
    MeisoCompositionHostRuntime *runtime,
    const MeisoCompositionMediaFrameRef *expected_frame
)
{
    MeisoCompositionHostRuntimeStatus status = meiso_composition_host_runtime_private_mutation_status(runtime);
    MeisoCompositionMediaTransportStatus media_status;
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) return status;
    if (expected_frame == NULL ||
        meiso_composition_host_runtime_private_range_overlap(runtime, expected_frame, sizeof(*expected_frame))) {
        return MEISO_COMPOSITION_HOST_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->active_media == NULL) {
        return MEISO_COMPOSITION_HOST_RUNTIME_NO_FRAME;
    }
    media_status = meiso_composition_media_transport_release_latest(
        runtime->active_media->transport, expected_frame
    );
    if (media_status == MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        return MEISO_COMPOSITION_HOST_RUNTIME_OK;
    }
    return media_status == MEISO_COMPOSITION_MEDIA_TRANSPORT_NO_FRAME ?
        MEISO_COMPOSITION_HOST_RUNTIME_NO_FRAME :
        MEISO_COMPOSITION_HOST_RUNTIME_FRAME_MISMATCH;
}
