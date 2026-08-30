#define _POSIX_C_SOURCE 200809L

#include "composition_edge_runtime_private.h"

/* Media binding, configuration, transport, and failure handling. */

static bool media_route_config(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoCompositionControlSessionSnapshot *control_session,
    size_t media_index,
    MeisoCompositionMediaRouteConfig *out_route
)
{
    const size_t actual_index = media_index == 0U ?
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA :
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA;
    const size_t route_index = media_index == 0U ?
        ROUTE_RENDER_MEDIA_DATA : ROUTE_CAMERA_MEDIA_DATA;
    const MeisoCompositionMediaActual *media = media_index == 0U ?
        &runtime->render_media_actual : &runtime->camera_media_actual;
    const MeisoCompositionMediaCapability *capability = media_index == 0U ?
        &runtime->catalog.render_media_capability :
        &runtime->catalog.camera_media_capability;
    const EdgeActualChannel *actual = &runtime->actual[actual_index];
    const MeisoRuntimeRouteDescriptor *descriptor =
        &runtime->catalog_routes[route_index].route;
    MeisoCompositionMediaRouteConfig route;
    if (out_route == NULL || !actual->snapshot.present ||
        !actual->snapshot.effective_enabled ||
        actual->snapshot.availability != MEISO_RUNTIME_AVAILABILITY_PRESENT ||
        actual->snapshot.production_status !=
            MEISO_RUNTIME_PRODUCTION_RUNNING ||
        actual->snapshot.production_epoch == 0U ||
        media->media_stream_epoch == 0U ||
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
    route.binding.production_epoch = actual->snapshot.production_epoch;
    route.binding.media_stream_epoch = media->media_stream_epoch;
    route.binding.channel_nonce = media->channel_nonce;
    route.capability = *capability;
    route.effective = media->effective;
    *out_route = route;
    return true;
}

static bool media_actual_running(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    size_t media_index
)
{
    const size_t actual_index = media_index == 0U ?
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA :
        MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA;
    const MeisoCompositionMediaActual *media = media_index == 0U ?
        &runtime->render_media_actual : &runtime->camera_media_actual;
    const EdgeActualChannel *actual = &runtime->actual[actual_index];
    return actual->snapshot.present && actual->snapshot.effective_enabled &&
        actual->snapshot.availability == MEISO_RUNTIME_AVAILABILITY_PRESENT &&
        actual->snapshot.production_status ==
            MEISO_RUNTIME_PRODUCTION_RUNNING &&
        actual->snapshot.production_epoch != 0U &&
        media->media_stream_epoch == actual->snapshot.production_epoch &&
        meiso_session_nonce_valid(media->channel_nonce);
}

static bool build_media_config(
    const MeisoK230dCompositionEdgeRuntime *runtime,
    bool require_delivery_accepted,
    MeisoCompositionMediaTransportConfig *out_config
)
{
    MeisoCompositionControlSessionSnapshot control_session;
    MeisoCompositionMediaTransportConfig config;
    size_t index;
    uint8_t intended_mask = 0U;
    if (out_config == NULL ||
        !meiso_composition_control_session_snapshot(runtime->control_session, &control_session) ||
        control_session.phase != MEISO_COMPOSITION_CONTROL_SESSION_PHASE_READY ||
        control_session.runtime.active_catalog_generation != runtime->config.catalog_generation ||
        control_session.session.state != MEISO_SESSION_STATE_READY ||
        !control_session.session.session_id_known) {
        return false;
    }
    memset(&config, 0, sizeof(config));
    config.local_role = MEISO_SESSION_ROLE_EDGE;
    config.scope.session_id = control_session.session.session_id;
    config.scope.catalog_generation = runtime->config.catalog_generation;
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        if (media_actual_running(runtime, index)) {
            intended_mask = (uint8_t)(intended_mask | bit);
        }
    }
    if (intended_mask == 0U) {
        return false;
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        const size_t actual_index = index == 0U ?
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA;
        if ((intended_mask & bit) != 0U) {
            if ((require_delivery_accepted &&
                 (!runtime->actual[actual_index].snapshot.delivery_accepted ||
                  runtime->actual[actual_index].snapshot.delivery_pending)) ||
                !media_route_config(
                    runtime, &control_session, index, &config.routes[index]
                )) {
                return false;
            }
        } else {
            config.routes[index].capability = index == 0U ?
                runtime->catalog.render_media_capability :
                runtime->catalog.camera_media_capability;
        }
    }
    config.active_route_mask = intended_mask;
    *out_config = config;
    return true;
}

static bool media_binding_equal(
    const MeisoCompositionMediaBinding *lhs,
    const MeisoCompositionMediaBinding *rhs
)
{
    return meiso_session_id_equal(lhs->session_id, rhs->session_id) &&
        lhs->catalog_generation == rhs->catalog_generation &&
        meiso_endpoint_id_equal(lhs->endpoint_id, rhs->endpoint_id) &&
        lhs->endpoint_incarnation == rhs->endpoint_incarnation &&
        meiso_resource_id_equal(lhs->resource_id, rhs->resource_id) &&
        lhs->source == rhs->source &&
        lhs->production_epoch == rhs->production_epoch &&
        lhs->media_stream_epoch == rhs->media_stream_epoch &&
        lhs->channel_nonce.high == rhs->channel_nonce.high &&
        lhs->channel_nonce.low == rhs->channel_nonce.low;
}

static bool active_media_scope_current(MeisoK230dCompositionEdgeRuntime *runtime)
{
    MeisoCompositionMediaTransportConfig current;
    size_t index;
    if (runtime->active_media == NULL ||
        !build_media_config(runtime, false, &current) ||
        current.active_route_mask != runtime->active_media->active_route_mask ||
        !meiso_session_id_equal(
            current.scope.session_id,
            runtime->active_media->transport_config.scope.session_id
        ) || current.scope.catalog_generation !=
            runtime->active_media->transport_config.scope.catalog_generation) {
        return false;
    }
    for (index = 0U; index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_ROUTE_COUNT;
         ++index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << index);
        if ((current.active_route_mask & bit) != 0U &&
            (!media_binding_equal(
                 &current.routes[index].binding,
                 &runtime->active_media->transport_config.routes[index].binding
             ) || !meiso_k230d_composition_edge_runtime_private_media_desired_equal(
                 &current.routes[index].effective,
                 &runtime->active_media->transport_config.routes[index].effective
             ))) {
            return false;
        }
    }
    return true;
}

void meiso_k230d_composition_edge_runtime_private_terminate_active_media(
    MeisoK230dCompositionEdgeRuntime *runtime,
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

static bool fail_active_media_production(
    MeisoK230dCompositionEdgeRuntime *runtime,
    bool unavailable
)
{
    uint8_t failed_mask;
    size_t media_index;
    if (runtime == NULL || runtime->active_media == NULL) {
        return false;
    }
    failed_mask = runtime->active_media->active_route_mask;
    meiso_k230d_composition_edge_runtime_private_terminate_active_media(
        runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST, 0
    );
    if ((failed_mask & MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_CAMERA) != 0U) {
        if (!meiso_k230d_composition_edge_runtime_private_stop_camera_bounded(runtime, false)) {
            meiso_k230d_composition_edge_runtime_private_enter_terminal(
                runtime, MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL_CAMERA_CLEANUP, 0
            );
            return false;
        }
        runtime->camera_frame_pending = false;
        runtime->camera_frame_submitted = false;
        memset(&runtime->camera_frame, 0, sizeof(runtime->camera_frame));
    }
    for (media_index = 0U;
         media_index < MEISO_K230D_COMPOSITION_EDGE_RUNTIME_MEDIA_ROUTE_COUNT;
         ++media_index) {
        const uint8_t bit = (uint8_t)(UINT8_C(1) << media_index);
        const size_t actual_index = media_index == 0U ?
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_RENDER_MEDIA :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_ACTUAL_CAMERA_MEDIA;
        EdgeActualChannel *actual;
        if ((failed_mask & bit) == 0U) {
            continue;
        }
        actual = &runtime->actual[actual_index];
        actual->snapshot.apply_result = MEISO_RUNTIME_APPLY_FAILED;
        actual->snapshot.availability = unavailable ?
            MEISO_RUNTIME_AVAILABILITY_UNAVAILABLE :
            MEISO_RUNTIME_AVAILABILITY_PRESENT;
        actual->snapshot.effective_enabled = false;
        actual->snapshot.production_status =
            MEISO_RUNTIME_PRODUCTION_FAILED;
        actual->snapshot.reason = unavailable ?
            MEISO_RUNTIME_PRODUCTION_REASON_UNAVAILABLE :
            MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED;
        actual->snapshot.production_epoch = 0U;
        if (media_index == 0U) {
            runtime->render_media_actual_present = false;
            runtime->render_media_sequence = 0U;
            meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->render_media_cadence);
        } else {
            runtime->camera_media_actual_present = false;
            runtime->camera_media_sequence = 0U;
            meiso_k230d_composition_edge_runtime_private_cadence_reset(&runtime->camera_media_cadence);
        }
        if (!meiso_k230d_composition_edge_runtime_private_publish_actual(runtime, actual_index)) {
            return false;
        }
    }
    return true;
}

MeisoK230dCompositionEdgeRuntimeStatus meiso_k230d_composition_edge_runtime_bind_media_fd(
    MeisoK230dCompositionEdgeRuntime *runtime,
    int connected_fd,
    MeisoK230dCompositionEdgeRuntimeMediaBindReceipt *out_receipt
)
{
    MeisoCompositionMediaTransportConfig transport_config;
    MeisoCompositionMediaPosixTcpConfig adapter_config;
    EdgeMediaSlot *slot;
    EdgeMediaSlot slot_before;
    MeisoK230dCompositionEdgeRuntimeMediaBindReceipt receipt;
    uint8_t *slot_bytes;
    size_t transport_bytes;
    size_t transport_alignment;
    size_t adapter_bytes;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime) || out_receipt == NULL ||
        meiso_k230d_composition_edge_runtime_private_edge_runtime_range_overlap(runtime, out_receipt, sizeof(*out_receipt)) ||
        connected_fd == runtime->config.control_connected_fd) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL;
    }
    if (runtime->mutable_active) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_BUSY;
    }
    if (runtime->active_media != NULL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_BUSY;
    }
    if (runtime->media_slots_consumed >= runtime->config.media_connection_slots) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_CAPACITY;
    }
    if (!build_media_config(runtime, true, &transport_config)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_NOT_READY;
    }
    if (!meiso_k230d_composition_edge_runtime_private_fd_preflight(connected_fd) || !meiso_k230d_composition_edge_runtime_private_fd_prepare_send_safety(connected_fd)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_ARGUMENT;
    }
    if (meiso_composition_media_transport_memory_required(
            &transport_config, &transport_bytes, &transport_alignment
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        transport_bytes > runtime->media_transport_bytes ||
        transport_alignment > alignof(max_align_t)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_CAPACITY;
    }
    slot = &runtime->media_slots[runtime->media_slots_consumed];
    slot_before = *slot;
    slot_bytes = runtime->arena_begin + slot->offset;
    if (meiso_composition_media_transport_init(
            &transport_config,
            (MeisoMutableBuffer){slot_bytes, runtime->media_transport_bytes},
            &slot->transport
        ) != MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) {
        memset(slot_bytes, 0, runtime->media_slot_bytes);
        *slot = slot_before;
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INTERNAL_FAILURE;
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
        adapter_bytes > EDGE_MEDIA_ADAPTER_RESERVE_BYTES ||
        meiso_composition_media_posix_tcp_init(
            slot_bytes + runtime->media_transport_bytes,
            EDGE_MEDIA_ADAPTER_RESERVE_BYTES, &adapter_config,
            &slot->adapter
        ) != MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK) {
        (void)meiso_composition_media_transport_terminate(
            slot->transport, &transport_config.scope,
            MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST
        );
        memset(slot_bytes, 0, runtime->media_slot_bytes);
        *slot = slot_before;
        return adapter_bytes == 0U ||
                adapter_bytes > EDGE_MEDIA_ADAPTER_RESERVE_BYTES ?
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_CAPACITY :
            MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INTERNAL_FAILURE;
    }
    slot->consumed = true;
    slot->fd = connected_fd;
    slot->active_route_mask = transport_config.active_route_mask;
    slot->transport_config = transport_config;
    ++runtime->media_slots_consumed;
    runtime->active_media = slot;
    memset(&receipt, 0, sizeof(receipt));
    receipt.slot_index = (uint8_t)(slot - runtime->media_slots);
    receipt.active_route_mask = transport_config.active_route_mask;
    receipt.session_id = transport_config.scope.session_id;
    receipt.catalog_generation = transport_config.scope.catalog_generation;
    *out_receipt = receipt;
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK;
}

MeisoK230dCompositionEdgeRuntimeStatus meiso_k230d_composition_edge_runtime_unbind_media_fd(
    MeisoK230dCompositionEdgeRuntime *runtime,
    int error_number
)
{
    (void)error_number;
    if (!meiso_k230d_composition_edge_runtime_private_edge_runtime_valid(runtime)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_ARGUMENT;
    }
    if (runtime->phase == MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PHASE_TERMINAL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_TERMINAL;
    }
    if (runtime->mutable_active) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_BUSY;
    }
    if (runtime->active_media == NULL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_INVALID_STATE;
    }
    meiso_k230d_composition_edge_runtime_private_terminate_active_media(
        runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE, error_number
    );
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_OK;
}

MeisoK230dCompositionEdgeRuntimePumpStatus meiso_k230d_composition_edge_runtime_private_pump_media(
    MeisoK230dCompositionEdgeRuntime *runtime,
    const MeisoK230dCompositionEdgeRuntimeIoReady *ready,
    MeisoK230dCompositionEdgeRuntimePumpResult *out_result
)
{
    MeisoCompositionMediaPosixTcpReadStatus read_status;
    MeisoCompositionMediaPosixTcpWriteStatus write_status;
    MeisoCompositionMediaPosixTcpPrepareStatus prepare_status;
    MeisoCompositionMediaPosixTcpSnapshot before;
    if (runtime->active_media == NULL) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
    }
    if (meiso_k230d_composition_edge_runtime_private_edge_runtime_source_work_outstanding(runtime)) {
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
    }
    if (!active_media_scope_current(runtime)) {
        meiso_k230d_composition_edge_runtime_private_terminate_active_media(
            runtime, MEISO_COMPOSITION_MEDIA_TERMINAL_PRODUCTION_CHANGED, 0
        );
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
    }
    if (ready->media_readable) {
        read_status = meiso_composition_media_posix_tcp_on_readable(
            runtime->active_media->adapter
        );
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK ||
            read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_WOULD_BLOCK;
        }
        if (read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST ||
            read_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INVALID_ARGUMENT) {
            if (!fail_active_media_production(
                    runtime,
                    read_status ==
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_TRANSPORT_LOST
                )) {
                return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_MEDIA_PROGRESS;
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
    }
    if (!meiso_composition_media_posix_tcp_snapshot(
            runtime->active_media->adapter, &before
        )) {
        (void)fail_active_media_production(runtime, false);
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
    }
    prepare_status = meiso_composition_media_posix_tcp_prepare_outbound(
        runtime->active_media->adapter
    );
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TERMINAL ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_TRANSPORT_INVALID ||
        prepare_status ==
            MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_INVALID_ARGUMENT) {
        if (!fail_active_media_production(runtime, false)) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
    }
    if (prepare_status == MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY) {
        if (!ready->media_writable) {
            if (!before.tx_packet_borrowed) {
                memset(out_result, 0, sizeof(*out_result));
                out_result->event_present = true;
                out_result->event =
                    MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_MEDIA_PROGRESS;
                return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
            }
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
        }
        write_status = meiso_composition_media_posix_tcp_on_writable(
            runtime->active_media->adapter
        );
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED) {
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_WOULD_BLOCK;
        }
        if (write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_INVALID ||
            write_status ==
                MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INVALID_ARGUMENT) {
            if (!fail_active_media_production(
                    runtime,
                    write_status ==
                        MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_TRANSPORT_LOST
                )) {
                return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_INTERNAL_FAILURE;
            }
            memset(out_result, 0, sizeof(*out_result));
            out_result->event_present = true;
            out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_BOUNDARY;
            return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
        }
        memset(out_result, 0, sizeof(*out_result));
        out_result->event_present = true;
        out_result->event = MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_MEDIA_PROGRESS;
        return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_PROGRESS;
    }
    return MEISO_K230D_COMPOSITION_EDGE_RUNTIME_PUMP_IDLE;
}
