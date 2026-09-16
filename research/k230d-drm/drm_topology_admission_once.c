#define _POSIX_C_SOURCE 200809L

#include "adapters/meiso_drm_libdrm_topology_private.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t ProbePolicyStatus;
enum {
    PROBE_POLICY_NOT_EVALUATED = 0,
    PROBE_POLICY_BLOCKED = 1,
    PROBE_POLICY_ELIGIBLE = 2
};

typedef uint8_t ProbeOverallStatus;
enum {
    PROBE_OVERALL_BLOCKED = 0,
    PROBE_OVERALL_ELIGIBLE = 1,
    PROBE_OVERALL_OPERATION_FAILED = 2
};

typedef struct {
    char bytes[MEISO_K230D_DRM_NATIVE_RECEIPT_CAPACITY];
    size_t size;
    bool valid;
} ReceiptBuffer;

static void receipt_append(ReceiptBuffer *buffer, const char *format, ...)
{
    int written;
    va_list arguments;
    if (buffer == NULL || format == NULL || !buffer->valid ||
        buffer->size >= sizeof(buffer->bytes)) {
        if (buffer != NULL) {
            buffer->valid = false;
        }
        return;
    }
    va_start(arguments, format);
    written = vsnprintf(
        buffer->bytes + buffer->size,
        sizeof(buffer->bytes) - buffer->size,
        format,
        arguments
    );
    va_end(arguments);
    if (written < 0 ||
        (size_t)written >= sizeof(buffer->bytes) - buffer->size) {
        buffer->valid = false;
        return;
    }
    buffer->size += (size_t)written;
}

static void receipt_append_json_string(
    ReceiptBuffer *buffer,
    const char *text,
    size_t capacity
)
{
    size_t index;
    bool terminated = false;
    receipt_append(buffer, "\"");
    for (index = 0U; index < capacity; ++index) {
        const unsigned char byte = (unsigned char)text[index];
        if (byte == 0U) {
            terminated = true;
            break;
        }
        if (byte == '"' || byte == '\\') {
            receipt_append(buffer, "\\%c", (int)byte);
        } else if (byte >= 0x20U && byte <= 0x7eU) {
            receipt_append(buffer, "%c", (int)byte);
        } else {
            receipt_append(buffer, "\\u%04x", (unsigned int)byte);
        }
    }
    if (!terminated) {
        buffer->valid = false;
    }
    receipt_append(buffer, "\"");
}

static void receipt_append_hex(
    ReceiptBuffer *buffer,
    const uint8_t *bytes,
    size_t size
)
{
    size_t index;
    receipt_append(buffer, "\"");
    for (index = 0U; index < size; ++index) {
        receipt_append(buffer, "%02x", (unsigned int)bytes[index]);
    }
    receipt_append(buffer, "\"");
}

static const char *overall_token(ProbeOverallStatus status)
{
    switch (status) {
        case PROBE_OVERALL_BLOCKED:
            return "ADMISSION_BLOCKED";
        case PROBE_OVERALL_ELIGIBLE:
            return "TOPOLOGY_ELIGIBLE_NOT_COMMIT_TESTED";
        default:
            return "OPERATION_FAILED";
    }
}

static const char *policy_token(ProbePolicyStatus status)
{
    switch (status) {
        case PROBE_POLICY_BLOCKED:
            return "ADMISSION_BLOCKED";
        case PROBE_POLICY_ELIGIBLE:
            return "TOPOLOGY_ELIGIBLE_NOT_COMMIT_TESTED";
        default:
            return "NOT_EVALUATED";
    }
}

static const char *reason_token(MeisoK230dDrmAdmissionReason reason)
{
    switch (reason) {
        case MEISO_K230D_DRM_ADMISSION_NONE:
            return "NONE";
        case MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY:
            return "CLIENT_CAPABILITY";
        case MEISO_K230D_DRM_ADMISSION_TOPOLOGY_CAPACITY:
            return "TOPOLOGY_CAPACITY";
        case MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL:
            return "TOPOLOGY_NONCANONICAL";
        case MEISO_K230D_DRM_ADMISSION_NO_ACTIVE_ROUTE:
            return "NO_ACTIVE_ROUTE";
        case MEISO_K230D_DRM_ADMISSION_MULTIPLE_ACTIVE_ROUTES:
            return "MULTIPLE_ACTIVE_ROUTES";
        case MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY:
            return "NO_UNUSED_RGB565_OVERLAY";
        case MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE:
            return "ZPOS_UNPROVABLE";
        case MEISO_K230D_DRM_ADMISSION_MODE_ENVELOPE:
            return "MODE_ENVELOPE";
        case MEISO_K230D_DRM_ADMISSION_BUFFER_ENVELOPE:
            return "BUFFER_ENVELOPE";
        case MEISO_K230D_DRM_ADMISSION_PAGE_FLIP_CLOCK_UNPROVABLE:
            return "PAGE_FLIP_CLOCK_UNPROVABLE";
        default:
            return NULL;
    }
}

static const char *stage_token(MeisoK230dDrmNativeStage stage)
{
    switch (stage) {
        case MEISO_K230D_DRM_NATIVE_STAGE_NONE:
            return "NONE";
        case MEISO_K230D_DRM_NATIVE_STAGE_OPEN:
            return "OPEN";
        case MEISO_K230D_DRM_NATIVE_STAGE_FD_CONTRACT:
            return "FD_CONTRACT";
        case MEISO_K230D_DRM_NATIVE_STAGE_MASTER_OBSERVATION:
            return "MASTER_OBSERVATION";
        case MEISO_K230D_DRM_NATIVE_STAGE_UNIVERSAL_PLANES_CAP:
            return "UNIVERSAL_PLANES_CAP";
        case MEISO_K230D_DRM_NATIVE_STAGE_ATOMIC_CAP:
            return "ATOMIC_CAP";
        case MEISO_K230D_DRM_NATIVE_STAGE_DRIVER_VERSION:
            return "DRIVER_VERSION";
        case MEISO_K230D_DRM_NATIVE_STAGE_TIMESTAMP_CAP:
            return "TIMESTAMP_CAP";
        case MEISO_K230D_DRM_NATIVE_STAGE_RESOURCES:
            return "RESOURCES";
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE_RESOURCES:
            return "PLANE_RESOURCES";
        case MEISO_K230D_DRM_NATIVE_STAGE_CONNECTOR:
            return "CONNECTOR";
        case MEISO_K230D_DRM_NATIVE_STAGE_ENCODER:
            return "ENCODER";
        case MEISO_K230D_DRM_NATIVE_STAGE_CRTC:
            return "CRTC";
        case MEISO_K230D_DRM_NATIVE_STAGE_CRTC_PROPERTIES:
            return "CRTC_PROPERTIES";
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE:
            return "PLANE";
        case MEISO_K230D_DRM_NATIVE_STAGE_PLANE_PROPERTIES:
            return "PLANE_PROPERTIES";
        case MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION:
            return "TRANSLATION";
        case MEISO_K230D_DRM_NATIVE_STAGE_CLOSE:
            return "CLOSE";
        default:
            return NULL;
    }
}

static const char *connector_kind(uint32_t connector_type, bool selected)
{
    if (!selected) {
        return "NONE";
    }
    switch (connector_type) {
        case 11U:
            return "HDMI_A";
        case 12U:
            return "HDMI_B";
        case 16U:
            return "DSI";
        default:
            return "OTHER";
    }
}

static const char *plane_type_token(MeisoK230dDrmPlaneType type)
{
    switch (type) {
        case MEISO_K230D_DRM_PLANE_TYPE_PRIMARY:
            return "PRIMARY";
        case MEISO_K230D_DRM_PLANE_TYPE_OVERLAY:
            return "OVERLAY";
        case MEISO_K230D_DRM_PLANE_TYPE_CURSOR:
            return "CURSOR";
        default:
            return "NONE";
    }
}

static bool observation_contract(
    MeisoK230dDrmNativeReadStatus returned_status,
    const MeisoK230dDrmNativeObservation *observation
)
{
    if (observation == NULL || returned_status != observation->status ||
        observation->status > MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED ||
        observation->operation_stage > MEISO_K230D_DRM_NATIVE_STAGE_CLOSE ||
        observation->libdrm_objects_released !=
            (observation->libdrm_objects_seen ==
             observation->libdrm_objects_released_count) ||
        !observation->libdrm_objects_released ||
        observation->libdrm_objects_released_count >
            observation->libdrm_objects_seen ||
        (observation->master_observation_supported &&
         !observation->master_observation_attempted) ||
        (observation->master_observed_while_open &&
         !observation->master_observation_supported) ||
        (observation->atomic_client_cap_set &&
         !observation->universal_planes_client_cap_set) ||
        (observation->topology_complete &&
         (!observation->topology_ready_for_evaluation ||
          !observation->all_plane_properties_complete)) ||
        (observation->topology_complete &&
         (observation->raw_connector_count >
              MEISO_K230D_DRM_MAX_CONNECTORS ||
          observation->raw_crtc_count > MEISO_K230D_DRM_MAX_CRTCS ||
          observation->raw_crtc_count > 32U ||
          observation->raw_plane_count > MEISO_K230D_DRM_MAX_PLANES)) ||
        (observation->primary_node_verified && !observation->fd_opened) ||
        ((observation->master_observation_attempted ||
          observation->universal_planes_client_cap_set ||
          observation->atomic_client_cap_set ||
          observation->topology_ready_for_evaluation) &&
         !observation->primary_node_verified) ||
        (observation->topology_ready_for_evaluation &&
         (!observation->master_observation_supported ||
          !observation->universal_planes_client_cap_set ||
          !observation->atomic_client_cap_set)) ||
        (observation->topology_ready_for_evaluation &&
         !observation->topology_complete &&
         ((observation->raw_connector_count <=
               MEISO_K230D_DRM_MAX_CONNECTORS &&
           observation->raw_crtc_count <= MEISO_K230D_DRM_MAX_CRTCS &&
           observation->raw_crtc_count <= 32U &&
           observation->raw_plane_count <= MEISO_K230D_DRM_MAX_PLANES) ||
          observation->all_plane_properties_complete)) ||
        (observation->fd_opened &&
         (!observation->close_attempted || !observation->close_consumed)) ||
        (!observation->fd_opened &&
         (observation->close_attempted || observation->close_consumed ||
          observation->device_closed)) ||
        (observation->device_closed &&
         (!observation->fd_opened || !observation->close_consumed))) {
        return false;
    }
    if (observation->status == MEISO_K230D_DRM_NATIVE_READ_COMPLETE) {
        if (!observation->fd_opened || !observation->primary_node_verified ||
            !observation->device_closed ||
            !observation->master_observation_supported ||
            !observation->universal_planes_client_cap_set ||
            !observation->atomic_client_cap_set ||
            !observation->topology_ready_for_evaluation ||
            observation->operation_stage != MEISO_K230D_DRM_NATIVE_STAGE_NONE ||
            observation->error != 0 ||
            observation->preclassified_reason !=
                MEISO_K230D_DRM_ADMISSION_NONE) {
            return false;
        }
    } else if (observation->status ==
               MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED) {
        if (!observation->fd_opened || !observation->primary_node_verified ||
            !observation->device_closed ||
            !observation->master_observation_supported ||
            observation->topology_ready_for_evaluation ||
            observation->topology_complete ||
            observation->operation_stage !=
                MEISO_K230D_DRM_NATIVE_STAGE_NONE ||
            observation->error != 0 ||
            (observation->preclassified_reason !=
                 MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY &&
             observation->preclassified_reason !=
                 MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL)) {
            return false;
        }
        if ((observation->preclassified_reason ==
                 MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY &&
             (observation->atomic_client_cap_set ||
              observation->raw_connector_count != 0U ||
              observation->raw_crtc_count != 0U ||
              observation->raw_plane_count != 0U)) ||
            (observation->preclassified_reason ==
                 MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL &&
             (!observation->universal_planes_client_cap_set ||
              !observation->atomic_client_cap_set ||
              observation->raw_connector_count >
                  MEISO_K230D_DRM_MAX_CONNECTORS ||
              observation->raw_crtc_count > MEISO_K230D_DRM_MAX_CRTCS ||
              observation->raw_crtc_count > 32U ||
              observation->raw_plane_count >
                  MEISO_K230D_DRM_MAX_PLANES)) ||
            observation->topology.connector_count !=
                observation->raw_connector_count ||
            observation->topology.crtc_count !=
                observation->raw_crtc_count ||
            observation->topology.plane_count !=
                observation->raw_plane_count) {
            return false;
        }
    } else if (observation->operation_stage ==
                   MEISO_K230D_DRM_NATIVE_STAGE_NONE ||
               observation->error <= 0 ||
               (observation->topology_ready_for_evaluation &&
                observation->operation_stage !=
                    MEISO_K230D_DRM_NATIVE_STAGE_CLOSE &&
                observation->operation_stage !=
                    MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION)) {
        return false;
    }
    if (observation->topology_ready_for_evaluation &&
        (observation->topology.connector_count !=
             observation->raw_connector_count ||
         observation->topology.crtc_count != observation->raw_crtc_count ||
         observation->topology.plane_count != observation->raw_plane_count)) {
        return false;
    }
    return true;
}

static void derive_counts(
    const MeisoK230dDrmNativeObservation *observation,
    const MeisoK230dDrmTopologyAdmission *admission,
    uint32_t *active_plane_count,
    uint32_t *unused_rgb565_overlay_count,
    uint32_t *immutable_zpos_candidate_count
)
{
    uint32_t index;
    *active_plane_count = 0U;
    *unused_rgb565_overlay_count = 0U;
    *immutable_zpos_candidate_count = 0U;
    if (!observation->topology_complete || !admission->route_selected) {
        return;
    }
    for (index = 0U; index < observation->topology.plane_count; ++index) {
        const MeisoK230dDrmPlane *plane = &observation->topology.planes[index];
        const bool compatible =
            (plane->possible_crtcs &
             (UINT32_C(1) << admission->crtc_index)) != 0U;
        const bool unused =
            plane->properties[MEISO_K230D_DRM_PLANE_FB_ID].value == 0U &&
            plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value == 0U;
        if (plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value ==
            observation->topology.crtcs[admission->crtc_index].crtc_id) {
            ++*active_plane_count;
        }
        if (plane->type == MEISO_K230D_DRM_PLANE_TYPE_OVERLAY &&
            plane->supports_rgb565 && compatible && unused) {
            ++*unused_rgb565_overlay_count;
            if (plane->type_immutable && plane->zpos_present &&
                plane->zpos_immutable) {
                ++*immutable_zpos_candidate_count;
            }
        }
    }
}

static bool build_receipt(
    ReceiptBuffer *buffer,
    ProbeOverallStatus overall,
    ProbePolicyStatus policy,
    MeisoK230dDrmAdmissionReason reason,
    MeisoK230dDrmNativeStage operation_stage,
    int error,
    const MeisoK230dDrmNativeObservation *observation,
    const MeisoK230dDrmTopologyAdmission *admission
)
{
    static const uint8_t ZERO_SHA[MEISO_K230D_DRM_SHA256_SIZE] = {0U};
    const MeisoK230dDrmTopology *topology = &observation->topology;
    const MeisoK230dDrmConnector *connector = NULL;
    const MeisoK230dDrmCrtc *crtc = NULL;
    const MeisoK230dDrmPlane *plane = NULL;
    const char *reason_text = reason_token(reason);
    const char *stage_text = stage_token(operation_stage);
    uint32_t active_plane_count;
    uint32_t unused_rgb565_overlay_count;
    uint32_t immutable_zpos_candidate_count;

    if (buffer == NULL || observation == NULL || admission == NULL ||
        reason_text == NULL || stage_text == NULL) {
        return false;
    }
    if (admission->route_selected) {
        if (admission->connector_index >= topology->connector_count ||
            admission->crtc_index >= topology->crtc_count) {
            return false;
        }
        connector = &topology->connectors[admission->connector_index];
        crtc = &topology->crtcs[admission->crtc_index];
    }
    if (admission->plane_selected) {
        if (admission->plane_index >= topology->plane_count) {
            return false;
        }
        plane = &topology->planes[admission->plane_index];
    }
    derive_counts(
        observation, admission, &active_plane_count,
        &unused_rgb565_overlay_count, &immutable_zpos_candidate_count
    );
    memset(buffer, 0, sizeof(*buffer));
    buffer->valid = true;
    receipt_append(
        buffer,
        "{\"type\":\"k230d_drm_topology_admission_probe\","
        "\"schema\":\"meiso.k230d_drm_topology_admission_probe.v1\","
        "\"verification\":\"NATIVE_LIBDRM_TOPOLOGY_PROBE\","
        "\"build_target\":\"k230d_linux\","
        "\"status\":\"%s\",\"policy_status\":\"%s\","
        "\"scope\":\"topology_policy_only\","
        "\"admission_reason\":\"%s\","
        "\"operation_stage\":\"%s\",\"error\":%d,"
        "\"driver_name\":",
        overall_token(overall), policy_token(policy), reason_text,
        stage_text, error
    );
    receipt_append_json_string(
        buffer, topology->driver_name, sizeof(topology->driver_name)
    );
    receipt_append(
        buffer,
        ",\"driver_version_major\":%u,\"driver_version_minor\":%u,"
        "\"driver_version_patch\":%u,"
        "\"page_flip_timestamps_monotonic\":%s,"
        "\"raw_connector_count\":%u,\"raw_crtc_count\":%u,"
        "\"raw_plane_count\":%u,\"topology_complete\":%s,"
        "\"topology_sha256\":",
        topology->driver_version_major,
        topology->driver_version_minor,
        topology->driver_version_patch,
        topology->page_flip_timestamps_monotonic ? "true" : "false",
        observation->raw_connector_count,
        observation->raw_crtc_count,
        observation->raw_plane_count,
        observation->topology_complete ? "true" : "false"
    );
    receipt_append_hex(
        buffer,
        admission->topology_hash_valid ? admission->topology_sha256 : ZERO_SHA,
        MEISO_K230D_DRM_SHA256_SIZE
    );
    receipt_append(
        buffer,
        ",\"topology_hash_valid\":%s,\"route_selected\":%s,"
        "\"connector_id\":%u,\"connector_kind\":\"%s\","
        "\"connector_type\":%u,\"connector_type_id\":%u,"
        "\"crtc_id\":%u,\"crtc_index\":%u,\"mode_name\":",
        admission->topology_hash_valid ? "true" : "false",
        admission->route_selected ? "true" : "false",
        connector == NULL ? 0U : connector->connector_id,
        connector_kind(
            connector == NULL ? 0U : connector->connector_type,
            connector != NULL
        ),
        connector == NULL ? 0U : connector->connector_type,
        connector == NULL ? 0U : connector->connector_type_id,
        crtc == NULL ? 0U : crtc->crtc_id,
        admission->route_selected ? admission->crtc_index : 0U
    );
    if (crtc == NULL) {
        receipt_append(buffer, "\"\"");
    } else {
        receipt_append_json_string(
            buffer, crtc->mode.name, sizeof(crtc->mode.name)
        );
    }
    receipt_append(
        buffer,
        ",\"mode_width\":%u,\"mode_height\":%u,"
        "\"minimum_rgb565_bytes\":%llu,\"plane_selected\":%s,"
        "\"plane_id\":%u,\"plane_type\":\"%s\","
        "\"plane_supports_rgb565\":%s,\"plane_zpos_present\":%s,"
        "\"plane_zpos_immutable\":%s,\"plane_zpos\":%lld,"
        "\"active_plane_count\":%u,"
        "\"unused_rgb565_overlay_count\":%u,"
        "\"immutable_zpos_candidate_count\":%u,"
        "\"all_plane_properties_complete\":%s,"
        "\"universal_planes_client_cap_set\":%s,"
        "\"atomic_client_cap_set\":%s,"
        "\"explicit_kms_mutation_calls\":0,"
        "\"display_state_unchanged_unproven\":true,"
        "\"explicit_set_master_called\":false,"
        "\"explicit_drop_master_called\":false,"
        "\"master_observation_attempted\":%s,"
        "\"master_observation_supported\":%s,"
        "\"master_observed_while_open\":%s,"
        "\"explicit_display_write_attempted\":false,"
        "\"property_write_calls\":0,\"dumb_create_calls\":0,"
        "\"framebuffer_create_calls\":0,"
        "\"atomic_test_attempted\":false,"
        "\"real_commit_attempted\":false,\"modeset_attempted\":false,"
        "\"kms_latched\":false,\"input_frame_observed\":false,"
        "\"libdrm_objects_seen\":%u,"
        "\"libdrm_objects_released_count\":%u,"
        "\"libdrm_objects_released\":%s,"
        "\"fd_opened\":%s,\"primary_node_verified\":%s,"
        "\"close_attempted\":%s,"
        "\"close_consumed\":%s,\"device_closed\":%s,"
        "\"actual_running_authorized\":false,"
        "\"physical_evidence\":false}\n",
        crtc == NULL ? 0U : crtc->mode.hdisplay,
        crtc == NULL ? 0U : crtc->mode.vdisplay,
        (unsigned long long)admission->minimum_output_bytes,
        admission->plane_selected ? "true" : "false",
        plane == NULL ? 0U : plane->plane_id,
        plane_type_token(
            plane == NULL ? MEISO_K230D_DRM_PLANE_TYPE_UNKNOWN : plane->type
        ),
        plane != NULL && plane->supports_rgb565 ? "true" : "false",
        plane != NULL && plane->zpos_present ? "true" : "false",
        plane != NULL && plane->zpos_immutable ? "true" : "false",
        (long long)(plane == NULL ? 0 : plane->zpos),
        active_plane_count,
        unused_rgb565_overlay_count,
        immutable_zpos_candidate_count,
        observation->all_plane_properties_complete ? "true" : "false",
        observation->universal_planes_client_cap_set ? "true" : "false",
        observation->atomic_client_cap_set ? "true" : "false",
        observation->master_observation_attempted ? "true" : "false",
        observation->master_observation_supported ? "true" : "false",
        observation->master_observed_while_open ? "true" : "false",
        observation->libdrm_objects_seen,
        observation->libdrm_objects_released_count,
        observation->libdrm_objects_released ? "true" : "false",
        observation->fd_opened ? "true" : "false",
        observation->primary_node_verified ? "true" : "false",
        observation->close_attempted ? "true" : "false",
        observation->close_consumed ? "true" : "false",
        observation->device_closed ? "true" : "false"
    );
    return buffer->valid;
}

int meiso_k230d_drm_topology_admission_once_run(
    FILE *out_stream,
    FILE *error_stream,
    int argument_count,
    char **arguments,
    MeisoK230dDrmNativeObserveFn observe
)
{
    MeisoK230dDrmNativeObservation observation;
    MeisoK230dDrmTopologyAdmission admission;
    MeisoK230dDrmNativeReadStatus read_status;
    MeisoK230dDrmTopologyAdmissionStatus admission_status;
    MeisoK230dDrmNativeStage operation_stage;
    MeisoK230dDrmAdmissionReason reason;
    ProbePolicyStatus policy = PROBE_POLICY_NOT_EVALUATED;
    ProbeOverallStatus overall = PROBE_OVERALL_OPERATION_FAILED;
    ReceiptBuffer receipt;
    size_t path_size;
    int operation_error;
    int return_code;

    if (out_stream == NULL || error_stream == NULL || out_stream == error_stream ||
        arguments == NULL || observe == NULL || argument_count != 3 ||
        arguments[0] == NULL || arguments[1] == NULL ||
        arguments[2] == NULL || strcmp(arguments[1], "--device") != 0 ||
        !meiso_k230d_drm_private_path_size(arguments[2], &path_size)) {
        if (error_stream != NULL) {
            (void)fputs("E_ARGUMENT\n", error_stream);
            (void)fflush(error_stream);
        }
        return 2;
    }
    (void)path_size;
    memset(&observation, 0, sizeof(observation));
    observation.status = MEISO_K230D_DRM_NATIVE_READ_INVALID_ARGUMENT;
    read_status = observe(arguments[2], &observation);
    memset(&admission, 0, sizeof(admission));
    operation_stage = observation.operation_stage;
    operation_error = observation.error;
    reason = MEISO_K230D_DRM_ADMISSION_NONE;
    if (!observation_contract(read_status, &observation)) {
        (void)fputs("E_PROVIDER_CONTRACT\n", error_stream);
        (void)fflush(error_stream);
        return 1;
    } else if (observation.topology_ready_for_evaluation) {
        admission_status = meiso_k230d_drm_private_evaluate_topology(
            &observation.topology, &admission
        );
        if (admission_status == MEISO_K230D_DRM_TOPOLOGY_ELIGIBLE) {
            policy = PROBE_POLICY_ELIGIBLE;
            reason = MEISO_K230D_DRM_ADMISSION_NONE;
        } else if (admission_status == MEISO_K230D_DRM_TOPOLOGY_BLOCKED &&
                   reason_token(admission.reason) != NULL) {
            policy = PROBE_POLICY_BLOCKED;
            reason = admission.reason;
        } else {
            operation_stage = MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION;
            operation_error = EPROTO;
        }
    } else if (observation.preclassified_reason ==
                   MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY ||
               observation.preclassified_reason ==
                   MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL) {
        policy = PROBE_POLICY_BLOCKED;
        reason = observation.preclassified_reason;
    }
    if (operation_stage != MEISO_K230D_DRM_NATIVE_STAGE_NONE ||
        operation_error != 0) {
        overall = PROBE_OVERALL_OPERATION_FAILED;
        if (operation_stage == MEISO_K230D_DRM_NATIVE_STAGE_NONE) {
            operation_stage = MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION;
        }
        if (operation_error <= 0) {
            operation_error = EIO;
        }
    } else if (policy == PROBE_POLICY_ELIGIBLE) {
        overall = PROBE_OVERALL_ELIGIBLE;
    } else if (policy == PROBE_POLICY_BLOCKED) {
        overall = PROBE_OVERALL_BLOCKED;
    } else {
        overall = PROBE_OVERALL_OPERATION_FAILED;
        operation_stage = MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION;
        operation_error = EPROTO;
    }
    if (!build_receipt(
            &receipt, overall, policy, reason, operation_stage,
            operation_error, &observation, &admission
        )) {
        return 1;
    }
    if (fwrite(receipt.bytes, 1U, receipt.size, out_stream) != receipt.size ||
        fflush(out_stream) != 0) {
        return 1;
    }
    return_code = overall == PROBE_OVERALL_OPERATION_FAILED ? 1 : 0;
    return return_code;
}

#if !defined(MEISO_K230D_DRM_TOPOLOGY_ADMISSION_ONCE_NO_MAIN)
int main(int argument_count, char **arguments)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGPIPE, &action, NULL) != 0) {
        return 1;
    }
    return meiso_k230d_drm_topology_admission_once_run(
        stdout, stderr, argument_count, arguments,
        meiso_k230d_drm_libdrm_observe
    );
}
#endif
