#include "display_ingress_simulated_service_private.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void digest_hex(const uint8_t digest[32U], char output[65U])
{
    static const char HEX[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < 32U; ++index) {
        output[index * 2U] = HEX[digest[index] >> 4U];
        output[index * 2U + 1U] = HEX[digest[index] & UINT8_C(0x0f)];
    }
    output[64U] = '\0';
}

static bool session_id_hex(MeisoSessionId id, char output[65U])
{
    return snprintf(
        output,
        65U,
        "%016" PRIx64 "%016" PRIx64 "%016" PRIx64 "%016" PRIx64,
        id.host_nonce.high,
        id.host_nonce.low,
        id.edge_nonce.high,
        id.edge_nonce.low
    ) == 64;
}

static bool write_line(const char *line)
{
    return line != NULL && fputs(line, stdout) >= 0 && fflush(stdout) == 0;
}

bool service_emit_failure(const Service *service)
{
    const char *role = service != NULL && service->role_name != NULL ?
        service->role_name : "host";
    const char *plane = service != NULL && service->failure_plane != NULL ?
        service->failure_plane : "application";
    const char *stage = service != NULL && service->failure_stage != NULL ?
        service->failure_stage : "bootstrap";
    char line[512U];
    const int size = snprintf(
        line,
        sizeof(line),
        "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V1\","
        "\"role\":\"%s\",\"status\":\"FAILURE\","
        "\"plane\":\"%s\",\"stage\":\"%s\","
        "\"carrier\":\"CONTROLLED_RAW_UDP\","
        "\"physical_evidence\":false,\"link_security\":false,"
        "\"clean_shutdown\":false}\n",
        role,
        plane,
        stage
    );

    return size > 0 && (size_t)size < sizeof(line) && write_line(line);
}

bool service_success_ready(const Service *service)
{
    const uint8_t *primary;

    if (service == NULL || !service->runtime_ready ||
        !service->catalog_active || !service->binding_ready ||
        !service->frame_complete || !service->evidence_complete ||
        !service->media_terminated || !service->close_clean ||
        !service->descriptors_closed || service->catalog_generation == 0U ||
        service->applied_desired_version == 0U ||
        service->actual_version == 0U || service->production_epoch == 0U ||
        service->media_stream_epoch != service->production_epoch ||
        service->frame_sequence == 0U ||
        (service->role == MEISO_SESSION_ROLE_EDGE &&
         !service->udp_armed_before_actual_write)) {
        return false;
    }
    primary = service->role == MEISO_SESSION_ROLE_HOST ?
        service->input_sha256 : service->received_sha256;
    return memcmp(
        primary, service->evidence_sha256, sizeof(service->evidence_sha256)
    ) == 0;
}

bool service_emit_success(const Service *service)
{
    char session[65U];
    char primary[65U];
    char evidence[65U];
    char line[1024U];
    int size;

    if (!service_success_ready(service) ||
        !session_id_hex(service->session_id, session)) {
        return false;
    }
    digest_hex(
        service->role == MEISO_SESSION_ROLE_HOST ?
            service->input_sha256 : service->received_sha256,
        primary
    );
    digest_hex(service->evidence_sha256, evidence);
    if (service->role == MEISO_SESSION_ROLE_HOST) {
        size = snprintf(
            line,
            sizeof(line),
            "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V1\","
            "\"role\":\"host\",\"status\":\"SUCCESS\","
            "\"session_id\":\"%s\",\"catalog_generation\":%" PRIu64 ","
            "\"applied_desired_version\":%" PRIu64 ","
            "\"actual_version\":%" PRIu64 ",\"production_epoch\":%" PRIu64 ","
            "\"media_stream_epoch\":%" PRIu64 ",\"frame_sequence\":%" PRIu64 ","
            "\"input_sha256\":\"%s\",\"evidence_input_sha256\":\"%s\","
            "\"evidence_class\":\"SIMULATED\","
            "\"connector\":\"UNSPECIFIED\","
            "\"carrier\":\"CONTROLLED_RAW_UDP\","
            "\"physical_evidence\":false,\"link_security\":false,"
            "\"clean_shutdown\":%s}\n",
            session,
            service->catalog_generation,
            service->applied_desired_version,
            service->actual_version,
            service->production_epoch,
            service->media_stream_epoch,
            service->frame_sequence,
            primary,
            evidence,
            service->close_clean ? "true" : "false"
        );
    } else {
        size = snprintf(
            line,
            sizeof(line),
            "{\"schema\":\"DISPLAY_INGRESS_SIMULATED_SERVICE_RECEIPT_V1\","
            "\"role\":\"edge\",\"status\":\"SUCCESS\","
            "\"session_id\":\"%s\",\"catalog_generation\":%" PRIu64 ","
            "\"applied_desired_version\":%" PRIu64 ","
            "\"actual_version\":%" PRIu64 ",\"production_epoch\":%" PRIu64 ","
            "\"media_stream_epoch\":%" PRIu64 ",\"frame_sequence\":%" PRIu64 ","
            "\"received_sha256\":\"%s\",\"evidence_input_sha256\":\"%s\","
            "\"evidence_class\":\"SIMULATED\","
            "\"connector\":\"UNSPECIFIED\","
            "\"udp_armed_before_actual_write\":%s,"
            "\"carrier\":\"CONTROLLED_RAW_UDP\","
            "\"physical_evidence\":false,\"link_security\":false,"
            "\"clean_shutdown\":%s}\n",
            session,
            service->catalog_generation,
            service->applied_desired_version,
            service->actual_version,
            service->production_epoch,
            service->media_stream_epoch,
            service->frame_sequence,
            primary,
            evidence,
            service->udp_armed_before_actual_write ? "true" : "false",
            service->close_clean ? "true" : "false"
        );
    }
    return size > 0 && (size_t)size < sizeof(line) && write_line(line);
}
