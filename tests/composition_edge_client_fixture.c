#include "composition_edge_client_fixture.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool bytes_all(const void *value, size_t size, uint8_t byte)
{
    const uint8_t *bytes = (const uint8_t *)value;
    size_t i;

    for (i = 0U; i < size; ++i) {
        if (bytes[i] != byte) {
            return false;
        }
    }
    return true;
}

bool meiso_test_composition_edge_bootstrap(MeisoEdgeHandle *handle)
{
    MeisoEdgeSnapshot snapshot;
    MeisoEdgeEndpointView endpoint;
    bool supported = true;

    memset(&snapshot, 0, sizeof(snapshot));
    if (meiso_edge_snapshot(handle, &snapshot) != MEISO_EDGE_OK ||
        snapshot.api_version != MEISO_HOST_EDGE_API_VERSION ||
        snapshot.phase != MEISO_EDGE_PHASE_BOOTSTRAPPING || snapshot.online ||
        snapshot.resource_graph_live || snapshot.catalog_generation != 0U ||
        snapshot.operations != 0U || snapshot.endpoint_count != 0U ||
        snapshot.resource_count != 0U) {
        return false;
    }
    memset(&endpoint, 0xa5, sizeof(endpoint));
    if (meiso_edge_endpoint_at(handle, 0U, &endpoint) !=
            MEISO_EDGE_NOT_READY ||
        !bytes_all(&endpoint, sizeof(endpoint), UINT8_C(0xa5))) {
        return false;
    }
    if (meiso_edge_operation_supported(
            handle, MEISO_EDGE_OPERATION_STATE_READ, &supported
        ) != MEISO_EDGE_NOT_READY || !supported) {
        return false;
    }
    return true;
}

bool meiso_test_composition_edge_submit_wifi(
    MeisoEdgeHandle *handle,
    MeisoEdgeCommandReceipt *out_receipt
)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
            MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM,
        500U
    };
    MeisoCompositionEdgeSnapshot snapshot;

    if (out_receipt == NULL ||
        meiso_composition_edge_set_wifi(
            handle, true, &parameters, out_receipt
        ) != MEISO_EDGE_OK ||
        out_receipt->command_id != 1U ||
        out_receipt->kind != MEISO_EDGE_COMMAND_SET_WIFI ||
        out_receipt->local_result != MEISO_EDGE_LOCAL_QUEUED ||
        out_receipt->replaced_command_present ||
        !out_receipt->desired_state_present ||
        out_receipt->desired_channel != MEISO_COMPOSITION_EDGE_DESIRED_WIFI ||
        out_receipt->desired_state_version != 1U ||
        out_receipt->plan_generation_present ||
        out_receipt->mesh_content_id_present ||
        meiso_composition_edge_snapshot(handle, &snapshot) != MEISO_EDGE_OK ||
        snapshot.edge.phase != MEISO_EDGE_PHASE_BOOTSTRAPPING ||
        !snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI].requested ||
        !snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI].enabled ||
        snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI].command_id !=
            out_receipt->command_id ||
        snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI].state_version !=
            1U ||
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].present ||
        snapshot.wifi_sample_present) {
        return false;
    }
    return true;
}

bool meiso_test_composition_edge_ready(
    MeisoEdgeHandle *handle,
    uint64_t continuity_serial,
    bool expect_sample
)
{
    static const MeisoEdgeOperationMask supported_operations[] = {
        MEISO_EDGE_OPERATION_STATE_READ,
        MEISO_EDGE_OPERATION_STATE_SUBMIT,
        MEISO_EDGE_OPERATION_STREAM_READ,
        MEISO_EDGE_OPERATION_BLOB_PUT,
        MEISO_EDGE_OPERATION_COMPOSITION,
        MEISO_EDGE_OPERATION_MEDIA_READ
    };
    static const MeisoEdgeOperationMask unsupported_operations[] = {
        MEISO_EDGE_OPERATION_STATE_WATCH,
        MEISO_EDGE_OPERATION_STREAM_PUBLISH,
        MEISO_EDGE_OPERATION_ACTION_INVOKE,
        MEISO_EDGE_OPERATION_BLOB_LOOKUP
    };
    MeisoCompositionEdgeSnapshot snapshot;
    size_t host_submit_count = 0U;
    size_t edge_read_count = 0U;
    size_t external_media_count = 0U;
    size_t i;

    if (meiso_composition_edge_snapshot(handle, &snapshot) != MEISO_EDGE_OK ||
        snapshot.edge.phase != MEISO_EDGE_PHASE_READY ||
        !snapshot.edge.online || !snapshot.edge.resource_graph_live ||
        snapshot.edge.continuity_serial != continuity_serial ||
        snapshot.edge.catalog_generation != 1U ||
        snapshot.edge.operations !=
            (MEISO_EDGE_OPERATION_STATE_READ |
             MEISO_EDGE_OPERATION_STATE_SUBMIT |
             MEISO_EDGE_OPERATION_STREAM_READ |
             MEISO_EDGE_OPERATION_BLOB_PUT |
             MEISO_EDGE_OPERATION_COMPOSITION |
             MEISO_EDGE_OPERATION_MEDIA_READ) ||
        snapshot.edge.endpoint_count !=
            MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT ||
        snapshot.edge.resource_count !=
            MEISO_COMPOSITION_PROFILE_ROUTE_COUNT) {
        return false;
    }
    for (i = 0U; i < sizeof(supported_operations) /
                            sizeof(supported_operations[0]); ++i) {
        bool supported = false;
        if (meiso_edge_operation_supported(
                handle, supported_operations[i], &supported
            ) != MEISO_EDGE_OK || !supported) {
            return false;
        }
    }
    for (i = 0U; i < sizeof(unsupported_operations) /
                            sizeof(unsupported_operations[0]); ++i) {
        bool supported = true;
        if (meiso_edge_operation_supported(
                handle, unsupported_operations[i], &supported
            ) != MEISO_EDGE_OK || supported) {
            return false;
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_ENDPOINT_COUNT; ++i) {
        MeisoEdgeEndpointView endpoint;
        if (meiso_edge_endpoint_at(handle, i, &endpoint) != MEISO_EDGE_OK ||
            !meiso_endpoint_id_valid(endpoint.id) ||
            endpoint.incarnation == 0U ||
            !meiso_schema_id_valid(endpoint.capability_schema.id) ||
            endpoint.capability_schema.version == 0U ||
            !meiso_byte_view_valid(endpoint.capability) ||
            endpoint.capability.size == 0U) {
            return false;
        }
    }
    for (i = 0U; i < MEISO_COMPOSITION_PROFILE_ROUTE_COUNT; ++i) {
        MeisoEdgeResourceView resource;
        if (meiso_edge_resource_at(handle, i, &resource) != MEISO_EDGE_OK ||
            !meiso_endpoint_id_valid(resource.endpoint_id) ||
            resource.endpoint_incarnation == 0U ||
            meiso_resource_descriptor_validate(&resource.resource) !=
                MEISO_DESCRIPTOR_VALID) {
            return false;
        }
        if (resource.access == MEISO_EDGE_RESOURCE_HOST_SUBMIT) {
            ++host_submit_count;
        } else if (resource.access == MEISO_EDGE_RESOURCE_EDGE_READ) {
            ++edge_read_count;
        } else {
            return false;
        }
        if (resource.payload_path == MEISO_EDGE_PAYLOAD_EXTERNAL_MEDIA) {
            if (resource.resource.kind != MEISO_RESOURCE_KIND_STREAM) {
                return false;
            }
            ++external_media_count;
        } else if (resource.payload_path != MEISO_EDGE_PAYLOAD_INLINE) {
            return false;
        }
    }
    if (host_submit_count != 7U || edge_read_count != 11U ||
        external_media_count != 2U ||
        !snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI]
             .delivery_accepted ||
        !snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].present ||
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].apply_result !=
            MEISO_RUNTIME_APPLY_APPLIED ||
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].production_status !=
            MEISO_RUNTIME_PRODUCTION_RUNNING ||
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].production_epoch !=
            77U ||
        !snapshot.wifi_effective_present ||
        snapshot.wifi_effective.selected_fields !=
            (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
             MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) ||
        snapshot.wifi_effective.sample_period_ms != 500U ||
        snapshot.wifi_sample_present != expect_sample) {
        return false;
    }
    if (expect_sample &&
        (snapshot.wifi_sample.selected_fields !=
             (MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE |
              MEISO_WIFI_TELEMETRY_FIELD_SIGNAL_MBM) ||
         snapshot.wifi_sample.present_fields !=
             snapshot.wifi_sample.selected_fields ||
         !snapshot.wifi_sample.link_state ||
         snapshot.wifi_sample.signal_mbm != -5100 ||
         !snapshot.inline_streams[MEISO_COMPOSITION_EDGE_INLINE_WIFI].present ||
         snapshot.inline_streams[
             MEISO_COMPOSITION_EDGE_INLINE_WIFI
         ].received_count != 1U)) {
        return false;
    }
    return true;
}

bool meiso_test_composition_edge_terminal(MeisoEdgeHandle *handle)
{
    const MeisoWifiTelemetryDesiredParameters parameters = {
        MEISO_WIFI_TELEMETRY_FIELD_LINK_STATE,
        500U
    };
    MeisoCompositionEdgeSnapshot snapshot;
    MeisoEdgeCommandReceipt receipt;
    bool supported = true;

    if (meiso_composition_edge_snapshot(handle, &snapshot) != MEISO_EDGE_OK ||
        snapshot.edge.phase != MEISO_EDGE_PHASE_TERMINAL ||
        snapshot.edge.terminal_reason != MEISO_EDGE_TERMINAL_LOCAL_ABORT ||
        snapshot.edge.online || snapshot.edge.resource_graph_live ||
        snapshot.edge.operations != 0U || snapshot.edge.endpoint_count != 0U ||
        snapshot.edge.resource_count != 0U ||
        snapshot.desired[MEISO_COMPOSITION_EDGE_DESIRED_WIFI].requested ||
        snapshot.actual[MEISO_COMPOSITION_EDGE_ACTUAL_WIFI].present ||
        snapshot.wifi_effective_present || snapshot.wifi_sample_present) {
        return false;
    }
    memset(&receipt, 0xa5, sizeof(receipt));
    if (meiso_composition_edge_set_wifi(
            handle, true, &parameters, &receipt
        ) != MEISO_EDGE_TERMINAL ||
        !bytes_all(&receipt, sizeof(receipt), UINT8_C(0xa5)) ||
        meiso_edge_operation_supported(
            handle, MEISO_EDGE_OPERATION_STATE_READ, &supported
        ) != MEISO_EDGE_TERMINAL || !supported) {
        return false;
    }
    return true;
}

bool meiso_test_composition_edge_camera_frame(
    MeisoEdgeHandle *handle,
    uint64_t continuity_serial,
    const uint8_t *expected_bytes,
    size_t expected_size
)
{
    MeisoEdgeMediaFrameView frame;
    MeisoEdgeMediaFrameView unchanged;
    MeisoEdgeMediaFrameKey wrong;

    if (expected_bytes == NULL || expected_size == 0U ||
        meiso_composition_edge_media_latest(
            handle, MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA, &frame
        ) != MEISO_EDGE_OK ||
        frame.key.continuity_serial != continuity_serial ||
        frame.key.source != MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA ||
        frame.key.media_stream_epoch != 602U ||
        frame.key.frame_sequence != 1U ||
        frame.source_time.clock_epoch != 10U ||
        frame.source_time.monotonic_ns != 2000U ||
        frame.applied_scene_owner.role != 0U ||
        frame.applied_scene_owner.incarnation != 0U ||
        frame.applied_scene_state_version != 0U ||
        frame.active_plan_generation != 0U || frame.world_generation != 0U ||
        frame.dependency_sequence != 0U ||
        frame.pixel_format != MEISO_COMPOSITION_MEDIA_PIXEL_NV12 ||
        frame.codec != MEISO_COMPOSITION_MEDIA_CODEC_RAW ||
        frame.width != 320U || frame.height != 240U ||
        frame.flags != MEISO_COMPOSITION_MEDIA_FRAME_KEY ||
        frame.access_unit.size != expected_size ||
        memcmp(frame.access_unit.data, expected_bytes, expected_size) != 0) {
        return false;
    }
    wrong = frame.key;
    ++wrong.frame_sequence;
    if (meiso_composition_edge_media_release(handle, &wrong) !=
            MEISO_EDGE_STALE ||
        meiso_composition_edge_media_release(handle, &frame.key) !=
            MEISO_EDGE_OK) {
        return false;
    }
    memset(&unchanged, 0xa5, sizeof(unchanged));
    if (meiso_composition_edge_media_latest(
            handle, MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA, &unchanged
        ) != MEISO_EDGE_NO_DATA ||
        !bytes_all(&unchanged, sizeof(unchanged), UINT8_C(0xa5))) {
        return false;
    }
    return true;
}
