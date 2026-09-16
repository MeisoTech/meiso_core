#include "meiso_drm_libdrm_topology_private.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(                                                      \
                stderr, "CHECK failed at %s:%d: %s\n",                       \
                __FILE__, __LINE__, #condition                                  \
            );                                                                  \
            return false;                                                       \
        }                                                                       \
    } while (0)

enum {
    TEST_OUTPUT_CAPACITY = 16384
};

static MeisoK230dDrmNativeObservation fake_observation;
static MeisoK230dDrmNativeReadStatus fake_status;
static uint32_t fake_calls;

static const char *const RECEIPT_KEYS[] = {
    "type", "schema", "verification", "build_target", "status",
    "policy_status", "scope", "admission_reason", "operation_stage",
    "error", "driver_name", "driver_version_major",
    "driver_version_minor", "driver_version_patch",
    "page_flip_timestamps_monotonic", "raw_connector_count",
    "raw_crtc_count", "raw_plane_count", "topology_complete",
    "topology_sha256", "topology_hash_valid", "route_selected",
    "connector_id", "connector_kind", "connector_type",
    "connector_type_id", "crtc_id", "crtc_index", "mode_name",
    "mode_width", "mode_height", "minimum_rgb565_bytes",
    "plane_selected", "plane_id", "plane_type", "plane_supports_rgb565",
    "plane_zpos_present", "plane_zpos_immutable", "plane_zpos",
    "active_plane_count", "unused_rgb565_overlay_count",
    "immutable_zpos_candidate_count", "all_plane_properties_complete",
    "universal_planes_client_cap_set", "atomic_client_cap_set",
    "explicit_kms_mutation_calls", "display_state_unchanged_unproven",
    "explicit_set_master_called",
    "explicit_drop_master_called", "master_observation_attempted",
    "master_observation_supported", "master_observed_while_open",
    "explicit_display_write_attempted", "property_write_calls",
    "dumb_create_calls",
    "framebuffer_create_calls", "atomic_test_attempted",
    "real_commit_attempted", "modeset_attempted", "kms_latched",
    "input_frame_observed", "libdrm_objects_seen",
    "libdrm_objects_released_count", "libdrm_objects_released",
    "fd_opened", "primary_node_verified", "close_attempted",
    "close_consumed", "device_closed",
    "actual_running_authorized", "physical_evidence"
};

static void fill_plane_properties(
    MeisoK230dDrmPlane *plane,
    uint32_t property_base,
    uint64_t framebuffer_id,
    uint64_t crtc_id
)
{
    uint32_t index;
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        plane->properties[index].property_id = property_base + index;
        plane->properties[index].value = 0U;
    }
    plane->properties[MEISO_K230D_DRM_PLANE_FB_ID].value = framebuffer_id;
    plane->properties[MEISO_K230D_DRM_PLANE_CRTC_ID].value = crtc_id;
}

static void reset_complete_observation(void)
{
    MeisoK230dDrmTopology *topology;
    memset(&fake_observation, 0, sizeof(fake_observation));
    fake_status = MEISO_K230D_DRM_NATIVE_READ_COMPLETE;
    fake_calls = 0U;
    fake_observation.status = fake_status;
    fake_observation.topology_ready_for_evaluation = true;
    fake_observation.topology_complete = true;
    fake_observation.universal_planes_client_cap_set = true;
    fake_observation.atomic_client_cap_set = true;
    fake_observation.master_observation_attempted = true;
    fake_observation.master_observation_supported = true;
    fake_observation.all_plane_properties_complete = true;
    fake_observation.fd_opened = true;
    fake_observation.primary_node_verified = true;
    fake_observation.close_attempted = true;
    fake_observation.close_consumed = true;
    fake_observation.device_closed = true;
    fake_observation.libdrm_objects_released = true;
    fake_observation.raw_connector_count = 1U;
    fake_observation.raw_crtc_count = 1U;
    fake_observation.raw_plane_count = 2U;
    fake_observation.libdrm_objects_seen = 21U;
    fake_observation.libdrm_objects_released_count = 21U;

    topology = &fake_observation.topology;
    memcpy(topology->driver_name, "fake-drm", sizeof("fake-drm"));
    topology->driver_version_major = 1U;
    topology->driver_version_minor = 2U;
    topology->driver_version_patch = 3U;
    topology->page_flip_timestamps_monotonic = true;
    topology->connector_count = 1U;
    topology->connectors[0].connector_id = 10U;
    topology->connectors[0].connector_type = 11U;
    topology->connectors[0].connector_type_id = 1U;
    topology->connectors[0].encoder_id = 20U;
    topology->connectors[0].crtc_id = 30U;
    topology->connectors[0].connected = true;
    topology->crtc_count = 1U;
    topology->crtcs[0].crtc_id = 30U;
    topology->crtcs[0].framebuffer_id = 500U;
    topology->crtcs[0].active = true;
    topology->crtcs[0].mode_valid = true;
    memcpy(topology->crtcs[0].mode.name, "4x4", sizeof("4x4"));
    topology->crtcs[0].mode.clock_khz = 1000U;
    topology->crtcs[0].mode.hdisplay = 4U;
    topology->crtcs[0].mode.hsync_start = 5U;
    topology->crtcs[0].mode.hsync_end = 6U;
    topology->crtcs[0].mode.htotal = 7U;
    topology->crtcs[0].mode.vdisplay = 4U;
    topology->crtcs[0].mode.vsync_start = 5U;
    topology->crtcs[0].mode.vsync_end = 6U;
    topology->crtcs[0].mode.vtotal = 7U;
    topology->plane_count = 2U;
    topology->planes[0].plane_id = 40U;
    topology->planes[0].possible_crtcs = 1U;
    topology->planes[0].type = MEISO_K230D_DRM_PLANE_TYPE_PRIMARY;
    topology->planes[0].type_immutable = true;
    topology->planes[0].supports_rgb565 = true;
    topology->planes[0].zpos_present = true;
    topology->planes[0].zpos_immutable = true;
    topology->planes[0].zpos = 0;
    fill_plane_properties(&topology->planes[0], 100U, 500U, 30U);
    topology->planes[1].plane_id = 50U;
    topology->planes[1].possible_crtcs = 1U;
    topology->planes[1].type = MEISO_K230D_DRM_PLANE_TYPE_OVERLAY;
    topology->planes[1].type_immutable = true;
    topology->planes[1].supports_rgb565 = true;
    topology->planes[1].zpos_present = true;
    topology->planes[1].zpos_immutable = true;
    topology->planes[1].zpos = 1;
    fill_plane_properties(&topology->planes[1], 200U, 0U, 0U);
}

static MeisoK230dDrmNativeReadStatus fake_observe(
    const char *device_path,
    MeisoK230dDrmNativeObservation *out_observation
)
{
    ++fake_calls;
    if (strcmp(device_path, "/sensitive/card0") != 0) {
        return MEISO_K230D_DRM_NATIVE_READ_INVALID_ARGUMENT;
    }
    *out_observation = fake_observation;
    return fake_status;
}

static bool read_stream(FILE *stream, char *bytes, size_t capacity)
{
    size_t size;
    CHECK(stream != NULL && bytes != NULL && capacity > 1U);
    CHECK(fflush(stream) == 0);
    CHECK(fseek(stream, 0L, SEEK_SET) == 0);
    size = fread(bytes, 1U, capacity - 1U, stream);
    CHECK(!ferror(stream));
    CHECK(size < capacity - 1U || feof(stream));
    bytes[size] = '\0';
    return true;
}

static uint32_t key_count(const char *receipt)
{
    uint32_t count = 0U;
    const char *cursor = receipt;
    while ((cursor = strstr(cursor, "\":")) != NULL) {
        ++count;
        cursor += 2;
    }
    return count;
}

static bool receipt_shape(const char *receipt)
{
    const char *cursor = receipt;
    size_t index;
    size_t size;
    CHECK(receipt != NULL);
    size = strlen(receipt);
    CHECK(size > 2U && receipt[0] == '{' && receipt[size - 2U] == '}' &&
          receipt[size - 1U] == '\n');
    CHECK(strchr(receipt, '\n') == receipt + size - 1U);
    CHECK(key_count(receipt) ==
          sizeof(RECEIPT_KEYS) / sizeof(RECEIPT_KEYS[0]));
    for (index = 0U; index < sizeof(RECEIPT_KEYS) / sizeof(RECEIPT_KEYS[0]);
         ++index) {
        char key[96];
        const char *found;
        CHECK(snprintf(key, sizeof(key), "\"%s\":", RECEIPT_KEYS[index]) > 0);
        found = strstr(cursor, key);
        CHECK(found != NULL);
        cursor = found + strlen(key);
    }
    CHECK(strstr(
        receipt,
        "\"verification\":\"NATIVE_LIBDRM_TOPOLOGY_PROBE\","
        "\"build_target\":\"k230d_linux\""
    ) != NULL);
    CHECK(strstr(
        receipt,
        "\"explicit_kms_mutation_calls\":0,"
        "\"display_state_unchanged_unproven\":true,"
        "\"explicit_set_master_called\":false,"
        "\"explicit_drop_master_called\":false"
    ) != NULL);
    CHECK(strstr(
        receipt,
        "\"explicit_display_write_attempted\":false,"
        "\"property_write_calls\":0,\"dumb_create_calls\":0,"
        "\"framebuffer_create_calls\":0,"
        "\"atomic_test_attempted\":false,"
        "\"real_commit_attempted\":false,\"modeset_attempted\":false,"
        "\"kms_latched\":false,\"input_frame_observed\":false"
    ) != NULL);
    CHECK(strstr(
        receipt,
        "\"actual_running_authorized\":false,"
        "\"physical_evidence\":false}\n"
    ) != NULL);
    CHECK(strstr(receipt, "/sensitive/card0") == NULL);
    return true;
}

static bool run_probe(
    int *out_return_code,
    char *out_bytes,
    size_t out_capacity,
    char *error_bytes,
    size_t error_capacity
)
{
    char *arguments[] = {
        (char *)"probe", (char *)"--device", (char *)"/sensitive/card0"
    };
    FILE *out_stream = tmpfile();
    FILE *error_stream = tmpfile();
    CHECK(out_return_code != NULL && out_stream != NULL && error_stream != NULL);
    *out_return_code = meiso_k230d_drm_topology_admission_once_run(
        out_stream, error_stream, 3, arguments, fake_observe
    );
    CHECK(read_stream(out_stream, out_bytes, out_capacity));
    CHECK(read_stream(error_stream, error_bytes, error_capacity));
    CHECK(fclose(out_stream) == 0);
    CHECK(fclose(error_stream) == 0);
    return true;
}

static bool test_eligible_and_blocked_receipts(void)
{
    char output[TEST_OUTPUT_CAPACITY];
    char errors[128];
    int return_code;

    reset_complete_observation();
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 0 && errors[0] == '\0' && fake_calls == 1U);
    CHECK(receipt_shape(output));
    CHECK(strstr(
        output,
        "\"status\":\"TOPOLOGY_ELIGIBLE_NOT_COMMIT_TESTED\","
        "\"policy_status\":\"TOPOLOGY_ELIGIBLE_NOT_COMMIT_TESTED\""
    ) != NULL);
    CHECK(strstr(output, "\"admission_reason\":\"NONE\"") != NULL);
    CHECK(strstr(
        output,
        "\"topology_sha256\":"
        "\"be58f2965fd97272619b824b15b303b01f0ea9c1ed358ba63b50f5e895ed9311\""
    ) != NULL);
    CHECK(strstr(output, "\"connector_kind\":\"HDMI_A\"") != NULL);
    CHECK(strstr(output, "\"primary_node_verified\":true") != NULL);
    CHECK(strstr(output, "\"plane_id\":50") != NULL);
    CHECK(strstr(output, "\"minimum_rgb565_bytes\":32") != NULL);
    CHECK(strstr(output, "\"physical_evidence\":false}\n") != NULL);

    reset_complete_observation();
    fake_observation.topology.planes[1].zpos_present = false;
    fake_observation.topology.planes[1].zpos_immutable = false;
    fake_observation.topology.planes[1].zpos = 0;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 0 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"status\":\"ADMISSION_BLOCKED\"") != NULL);
    CHECK(strstr(
        output, "\"admission_reason\":\"ZPOS_UNPROVABLE\""
    ) != NULL);
    CHECK(strstr(output, "\"route_selected\":true") != NULL);
    CHECK(strstr(output, "\"plane_selected\":false") != NULL);
    CHECK(strstr(output, "\"unused_rgb565_overlay_count\":1") != NULL);
    CHECK(strstr(output, "\"immutable_zpos_candidate_count\":0") != NULL);
    return true;
}

static bool test_preclassified_and_operation_failure(void)
{
    char output[TEST_OUTPUT_CAPACITY];
    char errors[128];
    int return_code;

    reset_complete_observation();
    memset(&fake_observation.topology, 0, sizeof(fake_observation.topology));
    fake_status = MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED;
    fake_observation.status = fake_status;
    fake_observation.topology_ready_for_evaluation = false;
    fake_observation.topology_complete = false;
    fake_observation.all_plane_properties_complete = false;
    fake_observation.preclassified_reason =
        MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY;
    fake_observation.universal_planes_client_cap_set = false;
    fake_observation.atomic_client_cap_set = false;
    fake_observation.raw_connector_count = 0U;
    fake_observation.raw_crtc_count = 0U;
    fake_observation.raw_plane_count = 0U;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 0 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"status\":\"ADMISSION_BLOCKED\"") != NULL);
    CHECK(strstr(
        output, "\"admission_reason\":\"CLIENT_CAPABILITY\""
    ) != NULL);

    reset_complete_observation();
    fake_status = MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED;
    fake_observation.status = fake_status;
    fake_observation.operation_stage = MEISO_K230D_DRM_NATIVE_STAGE_CLOSE;
    fake_observation.error = EIO;
    fake_observation.device_closed = false;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"status\":\"OPERATION_FAILED\"") != NULL);
    CHECK(strstr(
        output,
        "\"policy_status\":\"TOPOLOGY_ELIGIBLE_NOT_COMMIT_TESTED\""
    ) != NULL);
    CHECK(strstr(output, "\"operation_stage\":\"CLOSE\"") != NULL);
    CHECK(strstr(output, "\"device_closed\":false") != NULL);

    memset(&fake_observation, 0, sizeof(fake_observation));
    fake_status = MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED;
    fake_observation.status = fake_status;
    fake_observation.operation_stage = MEISO_K230D_DRM_NATIVE_STAGE_OPEN;
    fake_observation.error = ENOENT;
    fake_observation.libdrm_objects_released = true;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"policy_status\":\"NOT_EVALUATED\"") != NULL);
    CHECK(strstr(output, "\"operation_stage\":\"OPEN\"") != NULL);
    CHECK(strstr(output, "\"fd_opened\":false") != NULL);
    CHECK(strstr(output, "\"primary_node_verified\":false") != NULL);

    reset_complete_observation();
    fake_observation.raw_connector_count =
        MEISO_K230D_DRM_MAX_CONNECTORS + 1U;
    fake_observation.topology.connector_count =
        fake_observation.raw_connector_count;
    fake_observation.topology_complete = false;
    fake_observation.all_plane_properties_complete = false;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 0 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"status\":\"ADMISSION_BLOCKED\"") != NULL);
    CHECK(strstr(
        output, "\"admission_reason\":\"TOPOLOGY_CAPACITY\""
    ) != NULL);
    CHECK(strstr(output, "\"topology_complete\":false") != NULL);
    CHECK(strstr(output, "\"topology_hash_valid\":false") != NULL);
    return true;
}

static bool rejected_observer_contract(void)
{
    char output[TEST_OUTPUT_CAPACITY];
    char errors[128];
    int return_code;

    fake_calls = 0U;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && fake_calls == 1U);
    CHECK(output[0] == '\0');
    CHECK(strcmp(errors, "E_PROVIDER_CONTRACT\n") == 0);
    return true;
}

static bool test_observer_contract_failure_is_sanitized(void)
{
    memset(&fake_observation, 0, sizeof(fake_observation));
    fake_status = MEISO_K230D_DRM_NATIVE_READ_COMPLETE;
    fake_observation.status = fake_status;
    fake_observation.libdrm_objects_released = true;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_observation.master_observation_supported = false;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_observation.atomic_client_cap_set = false;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_observation.device_closed = false;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_status = MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED;
    fake_observation.status = fake_status;
    fake_observation.preclassified_reason =
        MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL;
    fake_observation.topology_ready_for_evaluation = false;
    fake_observation.topology_complete = false;
    fake_observation.raw_connector_count =
        MEISO_K230D_DRM_MAX_CONNECTORS + 1U;
    fake_observation.topology.connector_count =
        fake_observation.raw_connector_count;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_observation.raw_connector_count =
        MEISO_K230D_DRM_MAX_CONNECTORS + 1U;
    fake_observation.topology.connector_count =
        fake_observation.raw_connector_count;
    fake_observation.topology_complete = false;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_observation.raw_connector_count =
        MEISO_K230D_DRM_MAX_CONNECTORS + 1U;
    fake_observation.topology.connector_count =
        fake_observation.raw_connector_count;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_status = MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED;
    fake_observation.status = fake_status;
    fake_observation.preclassified_reason =
        MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_status = MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED;
    fake_observation.status = fake_status;
    fake_observation.preclassified_reason =
        MEISO_K230D_DRM_ADMISSION_CLIENT_CAPABILITY;
    fake_observation.topology_ready_for_evaluation = false;
    fake_observation.topology_complete = false;
    fake_observation.all_plane_properties_complete = false;
    fake_observation.atomic_client_cap_set = false;
    fake_observation.device_closed = false;
    CHECK(rejected_observer_contract());

    reset_complete_observation();
    fake_status = MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED;
    fake_observation.status = fake_status;
    fake_observation.operation_stage = MEISO_K230D_DRM_NATIVE_STAGE_OPEN;
    fake_observation.error = ENOENT;
    CHECK(rejected_observer_contract());
    return true;
}

static bool test_argument_failures(void)
{
    char *relative_arguments[] = {
        (char *)"probe", (char *)"--device", (char *)"relative/card0"
    };
    char output[128];
    char errors[128];
    FILE *out_stream = tmpfile();
    FILE *error_stream = tmpfile();
    int return_code;
    CHECK(out_stream != NULL && error_stream != NULL);
    reset_complete_observation();
    return_code = meiso_k230d_drm_topology_admission_once_run(
        out_stream, error_stream, 3, relative_arguments, fake_observe
    );
    CHECK(return_code == 2 && fake_calls == 0U);
    CHECK(read_stream(out_stream, output, sizeof(output)));
    CHECK(read_stream(error_stream, errors, sizeof(errors)));
    CHECK(output[0] == '\0' && strcmp(errors, "E_ARGUMENT\n") == 0);
    CHECK(fclose(out_stream) == 0 && fclose(error_stream) == 0);
    return true;
}

int main(void)
{
    if (!test_eligible_and_blocked_receipts() ||
        !test_preclassified_and_operation_failure() ||
        !test_observer_contract_failure_is_sanitized() ||
        !test_argument_failures()) {
        return 1;
    }
    (void)printf("K230D_DRM_TOPOLOGY_ADMISSION_RUNNER_TEST_PASSED\n");
    return 0;
}
