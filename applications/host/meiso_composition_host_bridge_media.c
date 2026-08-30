#include "meiso_composition_host_bridge_private.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* CMFB/v1 frame projection and Host Runtime media connection management. */

/*
 * CMFB/v1 is a local, big-endian 80-byte header followed by one copied access
 * unit: magic0, version4, header-size6, source8, format10, codec12, flags14,
 * width16, height18, payload-bytes20, sequence24, source clock/time32/40,
 * active plan48, world56 and applied Scene State version64; bytes72..79 zero.
 * It is not a Meiso wire frame and never crosses Runtime/Session.
 */

static bool checked_add_size(size_t lhs, size_t rhs, size_t *out)
{
    if (out == NULL || lhs > SIZE_MAX - rhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

static void media_retry_later(MeisoCompositionHostBridgePrivateProcess *service)
{
    if (!meiso_composition_host_bridge_private_deadline_after_ms(1000, &service->media_retry_after)) {
        memset(&service->media_retry_after, 0,
               sizeof(service->media_retry_after));
    }
}

static void write_be_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value >> 8U);
    output[1] = (uint8_t)value;
}

static void write_be_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value >> 24U);
    output[1] = (uint8_t)(value >> 16U);
    output[2] = (uint8_t)(value >> 8U);
    output[3] = (uint8_t)value;
}

static void write_be_u64(uint8_t *output, uint64_t value)
{
    write_be_u32(output, (uint32_t)(value >> 32U));
    write_be_u32(output + 4U, (uint32_t)value);
}

static bool queue_frame_output(
    MeisoCompositionHostBridgePrivateProcess *service,
    MeisoCompositionMediaSource source,
    bool *out_emitted
)
{
    MeisoEdgeMediaFrameView latest;
    MeisoEdgeStatus status;
    size_t record_size;
    char metadata[512];
    int written;

    *out_emitted = false;
    if (service->frame_output.pending) {
        return true;
    }
    status = meiso_composition_edge_media_latest(
        service->edge, source, &latest
    );
    if (status == MEISO_EDGE_NO_DATA) {
        return true;
    }
    if (status != MEISO_EDGE_OK ||
        (source == MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW &&
         latest.access_unit.size > service->config.catalog
             .render_media_capability.max_access_unit_bytes) ||
        (source == MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA &&
         latest.access_unit.size > service->config.catalog
             .camera_media_capability.max_access_unit_bytes) ||
        latest.access_unit.size > MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_FRAME_BYTES ||
        !checked_add_size(MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CMFB_HEADER_BYTES,
                          latest.access_unit.size, &record_size)) {
        return false;
    }
    memset(service->frame_output.bytes, 0, MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CMFB_HEADER_BYTES);
    memcpy(service->frame_output.bytes, "CMFB", 4U);
    write_be_u16(service->frame_output.bytes + 4U, 1U);
    write_be_u16(service->frame_output.bytes + 6U, MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CMFB_HEADER_BYTES);
    service->frame_output.bytes[8] = source;
    write_be_u16(service->frame_output.bytes + 10U, latest.pixel_format);
    write_be_u16(service->frame_output.bytes + 12U, latest.codec);
    write_be_u16(service->frame_output.bytes + 14U, latest.flags);
    write_be_u16(service->frame_output.bytes + 16U, latest.width);
    write_be_u16(service->frame_output.bytes + 18U, latest.height);
    write_be_u32(service->frame_output.bytes + 20U,
                 (uint32_t)latest.access_unit.size);
    write_be_u64(service->frame_output.bytes + 24U,
                 latest.key.frame_sequence);
    write_be_u64(service->frame_output.bytes + 32U,
                 latest.source_time.clock_epoch);
    write_be_u64(service->frame_output.bytes + 40U,
                 latest.source_time.monotonic_ns);
    write_be_u64(service->frame_output.bytes + 48U,
                 latest.active_plan_generation);
    write_be_u64(service->frame_output.bytes + 56U,
                 latest.world_generation);
    write_be_u64(service->frame_output.bytes + 64U,
                 latest.applied_scene_state_version);
    memcpy(service->frame_output.bytes + MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CMFB_HEADER_BYTES,
           latest.access_unit.data, latest.access_unit.size);
    service->frame_output.size = record_size;
    service->frame_output.offset = 0U;
    service->frame_output.pending = service->frame_fd >= 0;
    status = meiso_composition_edge_media_release(
        service->edge, &latest.key
    );
    if (status != MEISO_EDGE_OK) {
        memset(&service->frame_output, 0, sizeof(service->frame_output));
        return false;
    }
    written = snprintf(
        metadata, sizeof(metadata),
        "{\"type\":\"media.frame\",\"source\":\"%s\","
        "\"sequence\":%" PRIu64 ",\"clock_epoch\":%" PRIu64
        ",\"monotonic_ns\":%" PRIu64 ",\"pixel_format\":\"%s\","
        "\"codec\":\"%s\",\"width\":%u,\"height\":%u,"
        "\"access_unit_bytes\":%zu,\"frame_fd_queued\":%s}",
        meiso_composition_host_bridge_private_media_source_name(source), latest.key.frame_sequence,
        latest.source_time.clock_epoch,
        latest.source_time.monotonic_ns,
        meiso_composition_host_bridge_private_pixel_format_name(latest.pixel_format),
        meiso_composition_host_bridge_private_codec_name(latest.codec),
        (unsigned)latest.width, (unsigned)latest.height,
        latest.access_unit.size,
        service->frame_fd >= 0 ? "true" : "false"
    );
    if (written <= 0 || (size_t)written >= sizeof(metadata) ||
        !meiso_composition_host_bridge_private_emit_line(metadata)) {
        return false;
    }
    *out_emitted = true;
    return true;
}

bool meiso_composition_host_bridge_private_write_frame_output(MeisoCompositionHostBridgePrivateProcess *service, bool writable)
{
    ssize_t result;

    if (!service->frame_output.pending || !writable) {
        return true;
    }
    result = write(
        service->frame_fd,
        service->frame_output.bytes + service->frame_output.offset,
        service->frame_output.size - service->frame_output.offset
    );
    if (result > 0) {
        service->frame_output.offset += (size_t)result;
        if (service->frame_output.offset == service->frame_output.size) {
            service->frame_drain_cursor =
                service->frame_output.bytes[8] ==
                    MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW ? 1U : 0U;
            memset(&service->frame_output, 0, sizeof(service->frame_output));
        }
        return true;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                       errno == EINTR)) {
        return true;
    }
    return false;
}

static MeisoCompositionHostRuntimeMediaRouteMask current_media_mask(
    const MeisoCompositionHostRuntimeSnapshot *snapshot
)
{
    MeisoCompositionHostRuntimeMediaRouteMask mask = 0U;

    if (snapshot->media[0].actual_binding_ready) {
        mask |= MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER;
    }
    if (snapshot->media[1].actual_binding_ready) {
        mask |= MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA;
    }
    return mask;
}

static bool media_desired_channels_converged(
    const MeisoCompositionHostRuntimeSnapshot *snapshot
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
            &snapshot->desired[index];
        const MeisoCompositionHostRuntimeActualSnapshot *actual =
            &snapshot->actual[index];

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

bool meiso_composition_host_bridge_private_reconcile_media(MeisoCompositionHostBridgePrivateProcess *service)
{
    MeisoCompositionHostRuntimeSnapshot snapshot;
    MeisoCompositionHostRuntimeMediaRouteMask mask;
    MeisoCompositionHostRuntimeStatus status;
    int fd;
    bool connected;
    bool desired_converged;

    if (!meiso_composition_host_runtime_snapshot(service->host_runtime, &snapshot)) {
        return false;
    }
    if (service->media_fd >= 0 && !service->media_connecting &&
        !snapshot.media_bound) {
        meiso_composition_host_bridge_private_close_fd(&service->media_fd);
    }
    desired_converged = media_desired_channels_converged(&snapshot);
    mask = current_media_mask(&snapshot);
    if (service->media_connecting) {
        if (!desired_converged ||
            snapshot.phase != MEISO_COMPOSITION_HOST_RUNTIME_PHASE_READY ||
            mask == 0U || mask != service->media_connect_mask ||
            meiso_composition_host_bridge_private_time_reached(&service->media_connect_deadline)) {
            meiso_composition_host_bridge_private_close_fd(&service->media_fd);
            service->media_connecting = false;
            service->media_connect_mask = 0U;
            media_retry_later(service);
        }
        return true;
    }
    if (service->media_fd >= 0 || snapshot.media_bound ||
        snapshot.phase != MEISO_COMPOSITION_HOST_RUNTIME_PHASE_READY) {
        return true;
    }
    if (!desired_converged || mask == 0U ||
        snapshot.media_slots_consumed >= snapshot.media_slots_capacity ||
        !meiso_composition_host_bridge_private_time_reached(&service->media_retry_after)) {
        return true;
    }
    fd = meiso_composition_host_bridge_private_start_numeric_connect(&service->config.media_address,
                               service->config.media_address_size,
                               &connected);
    if (fd < 0) {
        media_retry_later(service);
        return true;
    }
    if (!connected) {
        service->media_fd = fd;
        service->media_connecting = true;
        service->media_connect_mask = mask;
        if (!meiso_composition_host_bridge_private_deadline_after_ms(MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_CONNECT_TIMEOUT_MS,
                               &service->media_connect_deadline)) {
            meiso_composition_host_bridge_private_close_fd(&service->media_fd);
            service->media_connecting = false;
            service->media_connect_mask = 0U;
            media_retry_later(service);
        }
        return true;
    }
    status = meiso_composition_host_runtime_bind_media_fd(service->host_runtime, fd,
                                                       mask);
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        meiso_composition_host_bridge_private_close_fd(&fd);
        media_retry_later(service);
        return status == MEISO_COMPOSITION_HOST_RUNTIME_NOT_READY ||
            status == MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE ||
            status == MEISO_COMPOSITION_HOST_RUNTIME_BUSY;
    }
    service->media_fd = fd;
    return meiso_composition_host_bridge_private_emit_line(
        "{\"type\":\"media.connected\",\"transport\":\"tcp\"}"
    );
}

bool meiso_composition_host_bridge_private_finish_media_connect(MeisoCompositionHostBridgePrivateProcess *service)
{
    MeisoCompositionHostRuntimeStatus status;
    int error_number = 0;
    socklen_t error_size = (socklen_t)sizeof(error_number);

    if (!service->media_connecting || service->media_fd < 0) {
        return false;
    }
    if (getsockopt(service->media_fd, SOL_SOCKET, SO_ERROR, &error_number,
                   &error_size) != 0 || error_number != 0) {
        meiso_composition_host_bridge_private_close_fd(&service->media_fd);
        service->media_connecting = false;
        service->media_connect_mask = 0U;
        media_retry_later(service);
        return true;
    }
    status = meiso_composition_host_runtime_bind_media_fd(
        service->host_runtime, service->media_fd, service->media_connect_mask
    );
    if (status != MEISO_COMPOSITION_HOST_RUNTIME_OK) {
        meiso_composition_host_bridge_private_close_fd(&service->media_fd);
        service->media_connecting = false;
        service->media_connect_mask = 0U;
        media_retry_later(service);
        return status == MEISO_COMPOSITION_HOST_RUNTIME_NOT_READY ||
            status == MEISO_COMPOSITION_HOST_RUNTIME_INVALID_STATE ||
            status == MEISO_COMPOSITION_HOST_RUNTIME_BUSY;
    }
    service->media_connecting = false;
    service->media_connect_mask = 0U;
    memset(&service->media_connect_deadline, 0,
           sizeof(service->media_connect_deadline));
    return meiso_composition_host_bridge_private_emit_line(
        "{\"type\":\"media.connected\",\"transport\":\"tcp\"}"
    );
}

bool meiso_composition_host_bridge_private_drain_latest_frames(MeisoCompositionHostBridgePrivateProcess *service)
{
    MeisoCompositionHostRuntimeSnapshot snapshot;
    MeisoCompositionEdgeSnapshot edge_snapshot;
    MeisoCompositionHostRuntimeMediaRouteMask active_mask = 0U;
    uint8_t scan;

    if (service->frame_output.pending) {
        return true;
    }
    if (service->backend == MEISO_COMPOSITION_HOST_BRIDGE_PRIVATE_BACKEND_SIMULATOR) {
        if (meiso_composition_edge_snapshot(service->edge, &edge_snapshot) !=
            MEISO_EDGE_OK) {
            return false;
        }
        if (edge_snapshot.actual[
                MEISO_COMPOSITION_EDGE_ACTUAL_RENDER_MEDIA
            ].present && edge_snapshot.actual[
                MEISO_COMPOSITION_EDGE_ACTUAL_RENDER_MEDIA
            ].effective_enabled) {
            active_mask |= MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER;
        }
        if (edge_snapshot.actual[
                MEISO_COMPOSITION_EDGE_ACTUAL_CAMERA_MEDIA
            ].present && edge_snapshot.actual[
                MEISO_COMPOSITION_EDGE_ACTUAL_CAMERA_MEDIA
            ].effective_enabled) {
            active_mask |= MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_CAMERA;
        }
    } else {
        if (!meiso_composition_host_runtime_snapshot(service->host_runtime, &snapshot)) {
            return false;
        }
        if (!snapshot.media_bound) {
            return true;
        }
        active_mask = snapshot.media_active_route_mask;
    }
    if (active_mask == 0U) {
        return true;
    }
    if ((active_mask &
         (uint8_t)(UINT8_C(1) << service->frame_drain_cursor)) == 0U) {
        service->frame_drain_cursor =
            (active_mask &
             MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_RENDER) != 0U ? 0U : 1U;
    }
    for (scan = 0U; scan < MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT;
         ++scan) {
        uint8_t route = (uint8_t)((service->frame_drain_cursor + scan) %
            MEISO_COMPOSITION_HOST_RUNTIME_MEDIA_ROUTE_COUNT);
        MeisoCompositionHostRuntimeMediaRouteMask route_mask =
            (MeisoCompositionHostRuntimeMediaRouteMask)(UINT8_C(1) << route);
        MeisoCompositionMediaSource source = route == 0U ?
            MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW :
            MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA;
        bool emitted;

        if ((active_mask & route_mask) == 0U) {
            continue;
        }
        if (!queue_frame_output(service, source, &emitted)) {
            return false;
        }
        if (emitted) {
            if (!service->frame_output.pending) {
                service->frame_drain_cursor = (uint8_t)(route ^ UINT8_C(1));
            }
            return true;
        }
    }
    return true;
}
