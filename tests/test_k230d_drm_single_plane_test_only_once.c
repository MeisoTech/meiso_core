#include "meiso_drm_libdrm_single_plane_test_only_private.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf( \
                stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition \
            ); \
            return false; \
        } \
    } while (0)

enum {
    TEST_OUTPUT_CAPACITY =
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_RECEIPT_CAPACITY + 64U
};

static const char ACCEPTED_RECEIPT_1[] =
    "{\"type\":\"k230d_drm_single_plane_test_only_probe\",\"schema\":\"meiso.k230d_drm_singl"
    "e_plane_test_only_probe.v1\",\"verification\":\"NATIVE_LIBDRM_TEST_ONLY_ONLY\",\"build"
    "_target\":\"k230d_linux\",\"scope\":\"single_plane_test_only_no_pixels\",\"outcome\":\"TES"
    "T_ACCEPTED_NOT_APPLIED\",\"primary_outcome\":\"TEST_ACCEPTED_NOT_APPLIED\",\"test_obse"
    "rvation\":\"ACCEPTED_NOT_APPLIED\",\"requested_topology_sha256\":\"0102030405060708090"
    "a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\",\"requested_connector_id\":10,\"requ"
    "ested_crtc_id\":30,\"requested_plane_id\":50,\"initial_candidate_reason\":\"NONE\",\"pre"
    "test_candidate_reason\":\"NONE\",\"posttest_candidate_reason\":\"NONE\",\"initial_topolo"
    "gy_sha256\":\"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\",\"p"
    "retest_topology_sha256\":\"0102030405060708090a0b0c0d0e0f101112131415161718191a1b1"
    "c1d1e1f20\",\"posttest_topology_sha256\":\"0102030405060708090a0b0c0d0e0f10111213141"
    "5161718191a1b1c1d1e1f20\",\"topology_read_attempt_count\":3,\"topology_read_success_"
    "count\":3,\"atomic_test_only_call_count\":1,\"initial_candidate_identified\":true,\"pr"
    "etest_candidate_identified\":true,\"posttest_candidate_identified\":true,\"pretest_t"
    "opology_unchanged\":true,\"posttest_topology_unchanged\":true,\"test_only_attempted\""
    ":true,\"test_callback_reported_accepted_not_applied\":true,\"test_error\":0,\"operati"
    "on_failure_stage\":\"NONE\",\"operation_error\":0,\"postcondition_failure_stage\":\"NONE"
    "\",\"postcondition_error\":0,\"cleanup_failure_stage\":\"NONE\",\"cleanup_error\":0,\"crea"
    "ted_dumb_handle\":101,\"created_dumb_pitch\":8,\"created_dumb_size\":32,\"created_fram"
    "ebuffer_id\":201,\"minimum_output_bytes\":32,\"fd_was_opened\":true,\"universal_planes"
    "_enabled\":true,\"atomic_enabled\":true,\"master_was_acquired\":true,\"master_verified"
    "\":true,\"dumb_was_created\":true,\"framebuffer_was_created\":true,\"remove_framebuffe"
    "r_attempted\":true,\"remove_framebuffer_succeeded\":true,\"destroy_dumb_attempted\":t"
    "rue,\"destroy_dumb_succeeded\":true,\"drop_master_attempted\":true,\"drop_master_succ"
    "eeded\":true,\"close_attempted\":true,\"close_succeeded\":true,\"fd_ownership_consumed"
    "\":true,\"master_ownership_consumed\":true,\"dumb_ownership_consumed\":true,\"framebuf"
    "fer_ownership_consumed\":true,\"native_open_primary_call_count\":1,\"native_open_pri"
    "mary_error\":0,\"native_fstat_call_count\":"
;

static const char ACCEPTED_RECEIPT_2[] =
    "1,\"native_primary_node_type_call_count\":1,\"native_fd_opened\":true,\"native_primar"
    "y_node_verified\":true,\"native_open_failure_close_call_count\":0,\"native_open_fail"
    "ure_close_error\":0,\"native_open_failure_close_terminal_consumed\":false,\"native_s"
    "ame_fd_contract_applicable\":true,\"native_same_fd_contract_verified\":true,\"native"
    "_same_fd_check_count\":14,\"native_universal_planes_cap_call_count\":1,\"native_univ"
    "ersal_planes_cap_error\":0,\"native_universal_planes_cap_set\":true,\"native_atomic_"
    "cap_call_count\":1,\"native_atomic_cap_error\":0,\"native_atomic_cap_set\":true,\"nati"
    "ve_set_master_call_count\":1,\"native_set_master_error\":0,\"native_is_master_call_c"
    "ount\":1,\"native_is_master_error\":0,\"native_master_was_acquired\":true,\"native_mas"
    "ter_verified\":true,\"native_topology_read_call_count\":3,\"native_topology_read_1_a"
    "ttempted\":true,\"native_topology_read_1_status\":\"COMPLETE\",\"native_topology_read_"
    "1_stage\":\"NONE\",\"native_topology_read_1_preclassified_reason\":0,\"native_topology"
    "_read_1_error\":0,\"native_topology_read_1_ready_for_evaluation\":true,\"native_topo"
    "logy_read_1_topology_complete\":true,\"native_topology_read_1_all_plane_properties"
    "_complete\":true,\"native_topology_read_1_raw_connector_count\":1,\"native_topology_"
    "read_1_raw_crtc_count\":1,\"native_topology_read_1_raw_plane_count\":1,\"native_topo"
    "logy_read_1_objects_seen\":12,\"native_topology_read_1_objects_released\":12,\"nativ"
    "e_topology_read_1_objects_all_released\":true,\"native_topology_read_2_attempted\":"
    "true,\"native_topology_read_2_status\":\"COMPLETE\",\"native_topology_read_2_stage\":\""
    "NONE\",\"native_topology_read_2_preclassified_reason\":0,\"native_topology_read_2_er"
    "ror\":0,\"native_topology_read_2_ready_for_evaluation\":true,\"native_topology_read_"
    "2_topology_complete\":true,\"native_topology_read_2_all_plane_properties_complete\""
    ":true,\"native_topology_read_2_raw_connector_count\":1,\"native_topology_read_2_raw"
    "_crtc_count\":1,\"native_topology_read_2_raw_plane_count\":1,\"native_topology_read_"
    "2_objects_seen\":12,\"native_topology_read_2_objects_released\":12,\"native_topology"
    "_read_2_objects_all_released\":true,\"native_topology_read_3_attempted\":true,\"nati"
    "ve_topology_read_3_status\":\"COMPLETE\",\"native_topology_read_3_stage\":\"NONE\",\"nat"
    "ive_topology_read_3_preclassified_reason"
;

static const char ACCEPTED_RECEIPT_3[] =
    "\":0,\"native_topology_read_3_error\":0,\"native_topology_read_3_ready_for_evaluatio"
    "n\":true,\"native_topology_read_3_topology_complete\":true,\"native_topology_read_3_"
    "all_plane_properties_complete\":true,\"native_topology_read_3_raw_connector_count\""
    ":1,\"native_topology_read_3_raw_crtc_count\":1,\"native_topology_read_3_raw_plane_c"
    "ount\":1,\"native_topology_read_3_objects_seen\":12,\"native_topology_read_3_objects"
    "_released\":12,\"native_topology_read_3_objects_all_released\":true,\"native_create_"
    "dumb_api_call_count\":1,\"native_create_dumb_error\":0,\"native_add_framebuffer_call"
    "_count\":1,\"native_add_framebuffer_error\":0,\"native_portable_atomic_callback_call"
    "_count\":1,\"native_atomic_request_alloc_call_count\":1,\"native_atomic_request_allo"
    "c_succeeded\":true,\"native_atomic_request_alloc_error\":0,\"native_atomic_plane_pro"
    "perty_add_attempt_count\":10,\"native_atomic_plane_property_add_success_count\":10,"
    "\"native_atomic_plane_property_add_error\":0,\"native_atomic_commit_api_call_count\""
    ":1,\"native_atomic_commit_flags\":256,\"native_atomic_commit_user_data_null\":true,\""
    "native_atomic_commit_error\":0,\"native_atomic_request_free_call_count\":1,\"native_"
    "remove_framebuffer_call_count\":1,\"native_remove_framebuffer_error\":0,\"native_des"
    "troy_dumb_api_call_count\":1,\"native_destroy_dumb_error\":0,\"native_drop_master_ca"
    "ll_count\":1,\"native_drop_master_error\":0,\"native_close_call_count\":1,\"native_clo"
    "se_error\":0,\"native_close_terminal_consumed\":true,\"native_map_call_count\":0,\"nat"
    "ive_pixel_write_call_count\":0,\"native_connector_property_add_call_count\":0,\"nati"
    "ve_crtc_property_add_call_count\":0,\"native_mode_blob_create_call_count\":0,\"nativ"
    "e_legacy_modeset_call_count\":0,\"native_non_test_atomic_commit_call_count\":0,\"nat"
    "ive_allow_modeset_flag_use_count\":0,\"native_page_flip_event_flag_use_count\":0,\"p"
    "ixel_payload_present\":false,\"framebuffer_mapped\":false,\"presented\":false,\"visibi"
    "lity_exclusivity_unproven\":true,\"real_commit_attempted\":false,\"allow_modeset_att"
    "empted\":false,\"page_flip_event_requested\":false,\"display_state_unchanged_unprove"
    "n\":true,\"display_actual_authorized\":false,\"physical_evidence\":false}\n"
;

static MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult fake_result;
static MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus fake_status;
static uint32_t fake_calls;
static bool fake_mutate_request;

static void fill_expected_hash(
    uint8_t hash[MEISO_K230D_DRM_SHA256_SIZE]
)
{
    uint32_t index;
    for (index = 0U; index < MEISO_K230D_DRM_SHA256_SIZE; ++index) {
        hash[index] = (uint8_t)(index + 1U);
    }
}

static MeisoK230dDrmSinglePlaneTestCandidateRequest fixture_request(void)
{
    MeisoK230dDrmSinglePlaneTestCandidateRequest request;
    memset(&request, 0, sizeof(request));
    fill_expected_hash(request.expected_topology_sha256);
    request.connector_id = 10U;
    request.crtc_id = 30U;
    request.plane_id = 50U;
    return request;
}

static void fill_complete_native_read(
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native,
    uint32_t slot,
    uint32_t plane_count
)
{
    native->topology_read_attempted[slot] = true;
    native->topology_read_status[slot] =
        MEISO_K230D_DRM_NATIVE_READ_COMPLETE;
    native->topology_read_stage[slot] = MEISO_K230D_DRM_NATIVE_STAGE_NONE;
    native->topology_read_preclassified_reason[slot] =
        MEISO_K230D_DRM_ADMISSION_NONE;
    native->topology_read_ready_for_evaluation[slot] = true;
    native->topology_read_topology_complete[slot] = true;
    native->topology_read_all_plane_properties_complete[slot] = true;
    native->topology_read_raw_connector_count[slot] = 1U;
    native->topology_read_raw_crtc_count[slot] = 1U;
    native->topology_read_raw_plane_count[slot] = plane_count;
    native->topology_read_objects_seen[slot] = 12U;
    native->topology_read_objects_released[slot] = 12U;
    native->topology_read_objects_all_released[slot] = true;
}

static void clear_native_read(
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native,
    uint32_t slot
)
{
    native->topology_read_attempted[slot] = false;
    native->topology_read_status[slot] =
        MEISO_K230D_DRM_NATIVE_READ_COMPLETE;
    native->topology_read_stage[slot] = MEISO_K230D_DRM_NATIVE_STAGE_NONE;
    native->topology_read_preclassified_reason[slot] =
        MEISO_K230D_DRM_ADMISSION_NONE;
    native->topology_read_error[slot] = 0;
    native->topology_read_ready_for_evaluation[slot] = false;
    native->topology_read_topology_complete[slot] = false;
    native->topology_read_all_plane_properties_complete[slot] = false;
    native->topology_read_raw_connector_count[slot] = 0U;
    native->topology_read_raw_crtc_count[slot] = 0U;
    native->topology_read_raw_plane_count[slot] = 0U;
    native->topology_read_objects_seen[slot] = 0U;
    native->topology_read_objects_released[slot] = 0U;
    native->topology_read_objects_all_released[slot] = false;
}

static void clear_native_atomic(
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    native->portable_atomic_callback_call_count = 0U;
    native->atomic_request_alloc_call_count = 0U;
    native->atomic_request_alloc_succeeded = false;
    native->atomic_request_alloc_error = 0;
    native->atomic_plane_property_add_attempt_count = 0U;
    native->atomic_plane_property_add_success_count = 0U;
    native->atomic_plane_property_add_error = 0;
    native->atomic_commit_api_call_count = 0U;
    native->atomic_commit_flags = 0U;
    native->atomic_commit_user_data_null = false;
    native->atomic_commit_error = 0;
    native->atomic_request_free_call_count = 0U;
}

static void fill_open_master_ledger(
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    native->open_primary_call_count = 1U;
    native->fstat_call_count = 1U;
    native->primary_node_type_call_count = 1U;
    native->native_fd_opened = true;
    native->primary_node_verified = true;
    native->same_fd_contract_applicable = true;
    native->same_fd_contract_verified = true;
    native->universal_planes_cap_call_count = 1U;
    native->atomic_cap_call_count = 1U;
    native->universal_planes_cap_set = true;
    native->atomic_cap_set = true;
    native->set_master_call_count = 1U;
    native->is_master_call_count = 1U;
    native->master_was_acquired = true;
    native->master_verified = true;
}

static void fill_clean_master_close(
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native
)
{
    native->drop_master_call_count = 1U;
    native->close_call_count = 1U;
    native->close_terminal_consumed = true;
}

static void reset_accepted_result(void)
{
    MeisoK230dDrmSinglePlaneTestCandidateRequest request = fixture_request();
    MeisoK230dDrmSinglePlaneTestOnlyResult *portable;
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native;
    uint32_t index;

    memset(&fake_result, 0, sizeof(fake_result));
    fake_status = MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE;
    fake_calls = 0U;
    fake_mutate_request = false;
    portable = &fake_result.portable;
    native = &fake_result.native;

    portable->outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_ACCEPTED_NOT_APPLIED;
    portable->primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED;
    portable->test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED;
    memcpy(
        portable->initial_topology_sha256,
        request.expected_topology_sha256,
        sizeof(portable->initial_topology_sha256)
    );
    memcpy(
        portable->pretest_topology_sha256,
        request.expected_topology_sha256,
        sizeof(portable->pretest_topology_sha256)
    );
    memcpy(
        portable->posttest_topology_sha256,
        request.expected_topology_sha256,
        sizeof(portable->posttest_topology_sha256)
    );
    portable->topology_read_attempt_count = 3U;
    portable->topology_read_success_count = 3U;
    portable->atomic_test_only_call_count = 1U;
    portable->connector_id = request.connector_id;
    portable->crtc_id = request.crtc_id;
    portable->plane_id = request.plane_id;
    portable->created_framebuffer_id = 201U;
    portable->created_dumb.handle = 101U;
    portable->created_dumb.pitch = 8U;
    portable->created_dumb.size = 32U;
    portable->minimum_output_bytes = 32U;
    portable->initial_candidate_identified = true;
    portable->pretest_candidate_identified = true;
    portable->posttest_candidate_identified = true;
    portable->pretest_topology_unchanged = true;
    portable->posttest_topology_unchanged = true;
    portable->test_only_attempted = true;
    portable->test_callback_reported_accepted_not_applied = true;
    portable->fd_was_opened = true;
    portable->universal_planes_enabled = true;
    portable->atomic_enabled = true;
    portable->master_was_acquired = true;
    portable->master_verified = true;
    portable->dumb_was_created = true;
    portable->framebuffer_was_created = true;
    portable->remove_framebuffer_attempted = true;
    portable->remove_framebuffer_succeeded = true;
    portable->destroy_dumb_attempted = true;
    portable->destroy_dumb_succeeded = true;
    portable->drop_master_attempted = true;
    portable->drop_master_succeeded = true;
    portable->close_attempted = true;
    portable->close_succeeded = true;
    portable->fd_ownership_consumed = true;
    portable->master_ownership_consumed = true;
    portable->dumb_ownership_consumed = true;
    portable->framebuffer_ownership_consumed = true;

    fill_open_master_ledger(native);
    native->topology_read_call_count = 3U;
    for (index = 0U; index < 3U; ++index) {
        fill_complete_native_read(native, index, 1U);
    }
    native->create_dumb_api_call_count = 1U;
    native->add_framebuffer_call_count = 1U;
    native->portable_atomic_callback_call_count = 1U;
    native->atomic_request_alloc_call_count = 1U;
    native->atomic_request_alloc_succeeded = true;
    native->atomic_plane_property_add_attempt_count =
        MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
    native->atomic_plane_property_add_success_count =
        MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
    native->atomic_commit_api_call_count = 1U;
    native->atomic_commit_flags =
        MEISO_K230D_DRM_NATIVE_TEST_ONLY_FLAG_VALUE;
    native->atomic_commit_user_data_null = true;
    native->atomic_request_free_call_count = 1U;
    native->remove_framebuffer_call_count = 1U;
    native->destroy_dumb_api_call_count = 1U;
    fill_clean_master_close(native);
    native->same_fd_check_count = 14U;
}

static void reset_initial_blocked_result(void)
{
    MeisoK230dDrmSinglePlaneTestCandidateRequest request = fixture_request();
    MeisoK230dDrmSinglePlaneTestOnlyResult *portable;
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native;

    memset(&fake_result, 0, sizeof(fake_result));
    fake_status = MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE;
    fake_calls = 0U;
    fake_mutate_request = false;
    portable = &fake_result.portable;
    native = &fake_result.native;

    portable->outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED;
    portable->primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED;
    portable->test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED;
    portable->initial_candidate_reason =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_MULTIPLE_TEST_TARGETS;
    memcpy(
        portable->initial_topology_sha256,
        request.expected_topology_sha256,
        sizeof(portable->initial_topology_sha256)
    );
    portable->topology_read_attempt_count = 1U;
    portable->topology_read_success_count = 1U;
    portable->connector_id = request.connector_id;
    portable->crtc_id = request.crtc_id;
    portable->plane_id = request.plane_id;
    portable->fd_was_opened = true;
    portable->universal_planes_enabled = true;
    portable->atomic_enabled = true;
    portable->master_was_acquired = true;
    portable->master_verified = true;
    portable->drop_master_attempted = true;
    portable->drop_master_succeeded = true;
    portable->close_attempted = true;
    portable->close_succeeded = true;
    portable->fd_ownership_consumed = true;
    portable->master_ownership_consumed = true;

    fill_open_master_ledger(native);
    native->topology_read_call_count = 1U;
    fill_complete_native_read(native, 0U, 2U);
    fill_clean_master_close(native);
    native->same_fd_check_count = 7U;
}

static void reset_open_failure_result(bool fd_was_obtained)
{
    MeisoK230dDrmSinglePlaneTestCandidateRequest request = fixture_request();
    MeisoK230dDrmSinglePlaneTestOnlyResult *portable;
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger *native;

    memset(&fake_result, 0, sizeof(fake_result));
    fake_status = MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE;
    fake_calls = 0U;
    fake_mutate_request = false;
    portable = &fake_result.portable;
    native = &fake_result.native;
    portable->outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
    portable->primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE;
    portable->test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED;
    portable->operation_failure_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY;
    portable->operation_error = ENODEV;
    portable->connector_id = request.connector_id;
    portable->crtc_id = request.crtc_id;
    portable->plane_id = request.plane_id;
    native->open_primary_call_count = 1U;
    native->open_primary_error = ENODEV;
    if (fd_was_obtained) {
        native->native_fd_opened = true;
        native->fstat_call_count = 1U;
        native->open_failure_close_call_count = 1U;
        native->open_failure_close_error = EIO;
        native->open_failure_close_terminal_consumed = true;
    }
}

static void reset_post_read_failure_result(void)
{
    reset_accepted_result();
    fake_result.portable.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
    fake_result.portable.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED;
    fake_result.portable.postcondition_failure_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY;
    fake_result.portable.postcondition_error = EIO;
    fake_result.portable.posttest_candidate_identified = false;
    fake_result.portable.posttest_topology_unchanged = false;
    memset(
        fake_result.portable.posttest_topology_sha256, 0,
        sizeof(fake_result.portable.posttest_topology_sha256)
    );
    fake_result.portable.topology_read_success_count = 2U;
    fake_result.native.topology_read_status[2] =
        MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED;
    fake_result.native.topology_read_stage[2] =
        MEISO_K230D_DRM_NATIVE_STAGE_RESOURCES;
    fake_result.native.topology_read_error[2] = EIO;
    fake_result.native.topology_read_ready_for_evaluation[2] = false;
    fake_result.native.topology_read_topology_complete[2] = false;
    fake_result.native.topology_read_all_plane_properties_complete[2] = false;
    fake_result.native.topology_read_raw_connector_count[2] = 0U;
    fake_result.native.topology_read_raw_crtc_count[2] = 0U;
    fake_result.native.topology_read_raw_plane_count[2] = 0U;
    fake_result.native.topology_read_objects_seen[2] = 1U;
    fake_result.native.topology_read_objects_released[2] = 1U;
}

static void reset_pretest_evaluation_failure_result(bool identified)
{
    reset_accepted_result();
    fake_result.portable.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
    fake_result.portable.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE;
    fake_result.portable.test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED;
    fake_result.portable.operation_failure_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE;
    fake_result.portable.operation_error = EPROTO;
    fake_result.portable.topology_read_attempt_count = 2U;
    fake_result.portable.topology_read_success_count = 2U;
    fake_result.portable.atomic_test_only_call_count = 0U;
    fake_result.portable.pretest_candidate_identified = identified;
    fake_result.portable.pretest_topology_unchanged = false;
    if (!identified) {
        memset(
            fake_result.portable.pretest_topology_sha256, 0,
            sizeof(fake_result.portable.pretest_topology_sha256)
        );
    }
    fake_result.portable.posttest_candidate_identified = false;
    fake_result.portable.posttest_topology_unchanged = false;
    memset(
        fake_result.portable.posttest_topology_sha256, 0,
        sizeof(fake_result.portable.posttest_topology_sha256)
    );
    fake_result.portable.test_only_attempted = false;
    fake_result.portable.test_callback_reported_accepted_not_applied = false;
    fake_result.native.topology_read_call_count = 2U;
    clear_native_read(&fake_result.native, 2U);
    clear_native_atomic(&fake_result.native);
    fake_result.native.same_fd_check_count = 12U;
}

static void reset_success_invalid_create_result(void)
{
    reset_accepted_result();
    fake_result.portable.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
    fake_result.portable.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE;
    fake_result.portable.test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED;
    fake_result.portable.operation_failure_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB;
    fake_result.portable.operation_error = EPROTO;
    fake_result.portable.topology_read_attempt_count = 1U;
    fake_result.portable.topology_read_success_count = 1U;
    fake_result.portable.atomic_test_only_call_count = 0U;
    fake_result.portable.pretest_candidate_identified = false;
    fake_result.portable.pretest_topology_unchanged = false;
    fake_result.portable.posttest_candidate_identified = false;
    fake_result.portable.posttest_topology_unchanged = false;
    memset(
        fake_result.portable.pretest_topology_sha256, 0,
        sizeof(fake_result.portable.pretest_topology_sha256)
    );
    memset(
        fake_result.portable.posttest_topology_sha256, 0,
        sizeof(fake_result.portable.posttest_topology_sha256)
    );
    fake_result.portable.test_only_attempted = false;
    fake_result.portable.test_callback_reported_accepted_not_applied = false;
    fake_result.portable.dumb_was_created = false;
    fake_result.portable.framebuffer_was_created = false;
    memset(
        &fake_result.portable.created_dumb, 0,
        sizeof(fake_result.portable.created_dumb)
    );
    fake_result.portable.created_framebuffer_id = 0U;
    fake_result.portable.remove_framebuffer_attempted = false;
    fake_result.portable.remove_framebuffer_succeeded = false;
    fake_result.portable.destroy_dumb_attempted = false;
    fake_result.portable.destroy_dumb_succeeded = false;
    fake_result.portable.dumb_ownership_consumed = false;
    fake_result.portable.framebuffer_ownership_consumed = false;
    fake_result.native.topology_read_call_count = 1U;
    clear_native_read(&fake_result.native, 1U);
    clear_native_read(&fake_result.native, 2U);
    fake_result.native.create_dumb_api_call_count = 1U;
    fake_result.native.create_dumb_error = 0;
    fake_result.native.add_framebuffer_call_count = 0U;
    fake_result.native.add_framebuffer_error = 0;
    clear_native_atomic(&fake_result.native);
    fake_result.native.remove_framebuffer_call_count = 0U;
    fake_result.native.destroy_dumb_api_call_count = 0U;
    fake_result.native.same_fd_check_count = 8U;
}

static void reset_success_invalid_add_result(void)
{
    reset_success_invalid_create_result();
    fake_result.portable.operation_failure_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER;
    fake_result.portable.dumb_was_created = true;
    fake_result.portable.created_dumb.handle = 101U;
    fake_result.portable.created_dumb.pitch = 8U;
    fake_result.portable.created_dumb.size = 32U;
    fake_result.portable.destroy_dumb_attempted = true;
    fake_result.portable.destroy_dumb_succeeded = true;
    fake_result.portable.dumb_ownership_consumed = true;
    fake_result.native.add_framebuffer_call_count = 1U;
    fake_result.native.destroy_dumb_api_call_count = 1U;
    fake_result.native.same_fd_check_count = 10U;
}

static MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus fake_execute(
    const char *device_path,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
)
{
    ++fake_calls;
    if (device_path == NULL || request == NULL || out_result == NULL ||
        strcmp(device_path, "/sensitive/card0") != 0) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT;
    }
    if (fake_mutate_request) {
        ((MeisoK230dDrmSinglePlaneTestCandidateRequest *)(void *)request)->
            plane_id = 51U;
    }
    memcpy(out_result, &fake_result, sizeof(*out_result));
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

static bool run_probe(
    int *out_return_code,
    char *output,
    size_t output_capacity,
    char *errors,
    size_t error_capacity
)
{
    static char hash[] =
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20";
    char *arguments[] = {
        (char *)"probe",
        (char *)"--device", (char *)"/sensitive/card0",
        (char *)"--topology-sha256", hash,
        (char *)"--connector-id", (char *)"10",
        (char *)"--crtc-id", (char *)"30",
        (char *)"--plane-id", (char *)"50"
    };
    FILE *out_stream = tmpfile();
    FILE *error_stream = tmpfile();

    CHECK(out_return_code != NULL && out_stream != NULL &&
          error_stream != NULL);
    *out_return_code = meiso_k230d_drm_single_plane_test_only_once_run(
        out_stream, error_stream, 11, arguments, fake_execute
    );
    CHECK(read_stream(out_stream, output, output_capacity));
    CHECK(read_stream(error_stream, errors, error_capacity));
    CHECK(fclose(out_stream) == 0 && fclose(error_stream) == 0);
    return true;
}

static bool receipt_shape(const char *receipt)
{
    size_t size;
    CHECK(receipt != NULL);
    size = strlen(receipt);
    CHECK(size > 2U && size <
          MEISO_K230D_DRM_NATIVE_TEST_ONLY_RECEIPT_CAPACITY);
    CHECK(receipt[0] == '{' && receipt[size - 2U] == '}' &&
          receipt[size - 1U] == '\n');
    CHECK(strchr(receipt, '\n') == receipt + size - 1U);
    CHECK(strncmp(
        receipt,
        "{\"type\":\"k230d_drm_single_plane_test_only_probe\","
        "\"schema\":\"meiso.k230d_drm_single_plane_test_only_probe.v1\","
        "\"verification\":\"NATIVE_LIBDRM_TEST_ONLY_ONLY\","
        "\"build_target\":\"k230d_linux\","
        "\"scope\":\"single_plane_test_only_no_pixels\",",
        strlen(
            "{\"type\":\"k230d_drm_single_plane_test_only_probe\","
            "\"schema\":\"meiso.k230d_drm_single_plane_test_only_probe.v1\","
            "\"verification\":\"NATIVE_LIBDRM_TEST_ONLY_ONLY\","
            "\"build_target\":\"k230d_linux\","
            "\"scope\":\"single_plane_test_only_no_pixels\","
        )
    ) == 0);
    CHECK(strstr(receipt, "/sensitive/card0") == NULL);
    CHECK(strstr(
        receipt,
        "\"pixel_payload_present\":false,"
        "\"framebuffer_mapped\":false,\"presented\":false,"
        "\"visibility_exclusivity_unproven\":true,"
        "\"real_commit_attempted\":false,"
        "\"allow_modeset_attempted\":false,"
        "\"page_flip_event_requested\":false,"
        "\"display_state_unchanged_unproven\":true,"
        "\"display_actual_authorized\":false,"
        "\"physical_evidence\":false}\n"
    ) != NULL);
    return true;
}

static bool accepted_receipt_exact(const char *receipt)
{
    const size_t size_1 = sizeof(ACCEPTED_RECEIPT_1) - 1U;
    const size_t size_2 = sizeof(ACCEPTED_RECEIPT_2) - 1U;
    const size_t size_3 = sizeof(ACCEPTED_RECEIPT_3) - 1U;
    CHECK(receipt != NULL);
    CHECK(strlen(receipt) == size_1 + size_2 + size_3);
    CHECK(memcmp(receipt, ACCEPTED_RECEIPT_1, size_1) == 0);
    CHECK(memcmp(receipt + size_1, ACCEPTED_RECEIPT_2, size_2) == 0);
    CHECK(memcmp(
        receipt + size_1 + size_2, ACCEPTED_RECEIPT_3, size_3
    ) == 0);
    return true;
}

static bool test_accepted_and_initial_blocked_receipts(void)
{
    char output[TEST_OUTPUT_CAPACITY];
    char errors[128];
    int return_code;

    reset_accepted_result();
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 0 && errors[0] == '\0' && fake_calls == 1U);
    CHECK(receipt_shape(output));
    CHECK(accepted_receipt_exact(output));
    CHECK(strstr(
        output,
        "\"outcome\":\"TEST_ACCEPTED_NOT_APPLIED\","
        "\"primary_outcome\":\"TEST_ACCEPTED_NOT_APPLIED\","
        "\"test_observation\":\"ACCEPTED_NOT_APPLIED\""
    ) != NULL);
    CHECK(strstr(
        output,
        "\"native_atomic_commit_api_call_count\":1,"
        "\"native_atomic_commit_flags\":256,"
        "\"native_atomic_commit_user_data_null\":true,"
        "\"native_atomic_commit_error\":0"
    ) != NULL);

    reset_initial_blocked_result();
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 0 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(
        output,
        "\"outcome\":\"NOT_TESTED\",\"primary_outcome\":\"NOT_TESTED\","
        "\"test_observation\":\"NOT_ATTEMPTED\""
    ) != NULL);
    CHECK(strstr(
        output,
        "\"initial_candidate_reason\":\"MULTIPLE_TEST_TARGETS\""
    ) != NULL);
    CHECK(strstr(
        output,
        "\"native_topology_read_2_attempted\":false,"
        "\"native_topology_read_2_status\":\"NOT_ATTEMPTED\""
    ) != NULL);
    CHECK(strstr(
        output, "\"native_atomic_commit_api_call_count\":0"
    ) != NULL);
    CHECK(strstr(
        output,
        "\"dumb_was_created\":false,"
        "\"framebuffer_was_created\":false,"
        "\"remove_framebuffer_attempted\":false"
    ) != NULL);
    CHECK(strstr(
        output,
        "\"native_create_dumb_api_call_count\":0,"
        "\"native_create_dumb_error\":0,"
        "\"native_add_framebuffer_call_count\":0"
    ) != NULL);
    CHECK(strstr(
        output,
        "\"native_remove_framebuffer_call_count\":0,"
        "\"native_remove_framebuffer_error\":0,"
        "\"native_destroy_dumb_api_call_count\":0"
    ) != NULL);
    return true;
}

static bool test_cleanup_failure_preserves_master_history(void)
{
    char output[TEST_OUTPUT_CAPACITY];
    char errors[128];
    int return_code;

    reset_accepted_result();
    fake_result.portable.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE;
    fake_result.portable.cleanup_failure_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER;
    fake_result.portable.cleanup_error = EIO;
    fake_result.portable.drop_master_succeeded = false;
    fake_result.native.drop_master_error = EIO;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"outcome\":\"CLEANUP_FAILURE\"") != NULL);
    CHECK(strstr(
        output,
        "\"cleanup_failure_stage\":\"DROP_MASTER\","
        "\"cleanup_error\":5"
    ) != NULL);
    CHECK(strstr(output, "\"master_was_acquired\":true") != NULL);
    CHECK(strstr(output, "\"master_ownership_consumed\":true") != NULL);
    CHECK(strstr(output, "\"native_master_was_acquired\":true") != NULL);
    CHECK(strstr(output, "\"native_drop_master_error\":5") != NULL);
    CHECK(strstr(output, "\"native_close_terminal_consumed\":true") != NULL);
    return true;
}

static bool test_valid_failure_receipts(void)
{
    char output[TEST_OUTPUT_CAPACITY];
    char errors[128];
    int return_code;

    reset_open_failure_result(false);
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"native_fd_opened\":false") != NULL);

    reset_open_failure_result(true);
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"native_fd_opened\":true") != NULL);
    CHECK(strstr(
        output,
        "\"native_open_failure_close_call_count\":1,"
        "\"native_open_failure_close_error\":5,"
        "\"native_open_failure_close_terminal_consumed\":true"
    ) != NULL);

    reset_accepted_result();
    fake_result.portable.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_REJECTED;
    fake_result.portable.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED;
    fake_result.portable.test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED;
    fake_result.portable.test_callback_reported_accepted_not_applied = false;
    fake_result.portable.test_error = EINVAL;
    fake_result.native.atomic_commit_error = EINVAL;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 0 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(output, "\"outcome\":\"TEST_REJECTED\"") != NULL);

    reset_accepted_result();
    fake_result.portable.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
    fake_result.portable.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE;
    fake_result.portable.test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED;
    fake_result.portable.test_callback_reported_accepted_not_applied = false;
    fake_result.portable.test_error = ENOMEM;
    fake_result.portable.operation_failure_stage =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY;
    fake_result.portable.operation_error = ENOMEM;
    fake_result.native.atomic_request_alloc_succeeded = false;
    fake_result.native.atomic_request_alloc_error = ENOMEM;
    fake_result.native.atomic_plane_property_add_attempt_count = 0U;
    fake_result.native.atomic_plane_property_add_success_count = 0U;
    fake_result.native.atomic_commit_api_call_count = 0U;
    fake_result.native.atomic_commit_flags = 0U;
    fake_result.native.atomic_commit_user_data_null = false;
    fake_result.native.atomic_request_free_call_count = 0U;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(
        output,
        "\"native_portable_atomic_callback_call_count\":1,"
        "\"native_atomic_request_alloc_call_count\":1,"
        "\"native_atomic_request_alloc_succeeded\":false"
    ) != NULL);

    reset_post_read_failure_result();
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(
        output, "\"postcondition_failure_stage\":\"READ_TOPOLOGY\""
    ) != NULL);

    reset_pretest_evaluation_failure_result(false);
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(
        output,
        "\"operation_failure_stage\":\"EVALUATE_PRETEST_CANDIDATE\""
    ) != NULL);
    CHECK(strstr(
        output, "\"pretest_candidate_identified\":false"
    ) != NULL);

    reset_pretest_evaluation_failure_result(true);
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(
        output,
        "\"pretest_candidate_identified\":true,"
        "\"posttest_candidate_identified\":false,"
        "\"pretest_topology_unchanged\":false"
    ) != NULL);

    reset_success_invalid_create_result();
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(
        output,
        "\"operation_failure_stage\":\"CREATE_DUMB\""
    ) != NULL);
    CHECK(strstr(
        output,
        "\"native_create_dumb_api_call_count\":1,"
        "\"native_create_dumb_error\":0"
    ) != NULL);

    reset_success_invalid_add_result();
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && errors[0] == '\0' && receipt_shape(output));
    CHECK(strstr(
        output,
        "\"operation_failure_stage\":\"ADD_FRAMEBUFFER\""
    ) != NULL);
    CHECK(strstr(
        output,
        "\"native_add_framebuffer_call_count\":1,"
        "\"native_add_framebuffer_error\":0"
    ) != NULL);
    return true;
}

static bool expect_provider_failure(void)
{
    char output[TEST_OUTPUT_CAPACITY];
    char errors[128];
    int return_code;
    CHECK(run_probe(
        &return_code, output, sizeof(output), errors, sizeof(errors)
    ));
    CHECK(return_code == 1 && fake_calls == 1U && output[0] == '\0');
    CHECK(strcmp(errors, "E_PROVIDER_CONTRACT\n") == 0);
    return true;
}

static bool test_provider_contract_failures(void)
{
    static const MeisoK230dDrmSinglePlaneTestOnlyOperationStage
        IMPOSSIBLE_BLOCKED_STAGES[] = {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_INITIAL_CANDIDATE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_EVALUATE_PRETEST_CANDIDATE
        };
    size_t index;

    reset_accepted_result();
    fake_mutate_request = true;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.native.master_was_acquired = false;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.portable.topology_read_success_count = 2U;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.native.topology_read_topology_complete[0] = false;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.portable.test_error = EINVAL;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.native.drop_master_error = EIO;
    fake_result.portable.drop_master_succeeded = false;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.native.topology_read_status[0] =
        MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED;
    fake_result.native.topology_read_stage[0] =
        MEISO_K230D_DRM_NATIVE_STAGE_RESOURCES;
    fake_result.native.topology_read_error[0] = EIO;
    fake_result.native.topology_read_ready_for_evaluation[0] = false;
    fake_result.native.topology_read_topology_complete[0] = false;
    fake_result.native.topology_read_all_plane_properties_complete[0] = false;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.native.topology_read_ready_for_evaluation[2] = true;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.native.topology_read_raw_connector_count[2] =
        MEISO_K230D_DRM_MAX_CONNECTORS + 1U;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.portable.postcondition_error = EPROTO;
    fake_result.native.topology_read_status[2] =
        MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED;
    fake_result.native.topology_read_stage[2] =
        MEISO_K230D_DRM_NATIVE_STAGE_NONE;
    fake_result.native.topology_read_preclassified_reason[2] =
        MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL;
    fake_result.native.topology_read_error[2] = 0;
    fake_result.native.topology_read_raw_plane_count[2] =
        MEISO_K230D_DRM_MAX_PLANES + 1U;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.native.topology_read_stage[2] =
        MEISO_K230D_DRM_NATIVE_STAGE_DRIVER_VERSION;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.native.topology_read_stage[2] =
        MEISO_K230D_DRM_NATIVE_STAGE_PLANE_RESOURCES;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.native.topology_read_stage[2] =
        MEISO_K230D_DRM_NATIVE_STAGE_CRTC;
    fake_result.native.topology_read_objects_seen[2] = 3U;
    fake_result.native.topology_read_objects_released[2] = 3U;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.portable.postcondition_error = EPROTO;
    fake_result.native.topology_read_status[2] =
        MEISO_K230D_DRM_NATIVE_READ_PRECLASSIFIED;
    fake_result.native.topology_read_stage[2] =
        MEISO_K230D_DRM_NATIVE_STAGE_NONE;
    fake_result.native.topology_read_preclassified_reason[2] =
        MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL;
    fake_result.native.topology_read_error[2] = 0;
    fake_result.native.topology_read_raw_connector_count[2] = 1U;
    CHECK(expect_provider_failure());

    reset_post_read_failure_result();
    fake_result.native.topology_read_stage[2] =
        MEISO_K230D_DRM_NATIVE_STAGE_TRANSLATION;
    fake_result.native.topology_read_objects_seen[2] = 3U;
    fake_result.native.topology_read_objects_released[2] = 3U;
    CHECK(expect_provider_failure());

    for (index = 0U;
         index < sizeof(IMPOSSIBLE_BLOCKED_STAGES) /
             sizeof(IMPOSSIBLE_BLOCKED_STAGES[0]);
         ++index) {
        reset_initial_blocked_result();
        fake_result.portable.outcome =
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE;
        fake_result.portable.primary_outcome =
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE;
        fake_result.portable.operation_failure_stage =
            IMPOSSIBLE_BLOCKED_STAGES[index];
        fake_result.portable.operation_error = EPROTO;
        CHECK(expect_provider_failure());
    }

    reset_accepted_result();
    fake_result.native.atomic_request_alloc_succeeded = false;
    fake_result.native.atomic_request_alloc_error = EINVAL;
    fake_result.native.atomic_plane_property_add_attempt_count = 0U;
    fake_result.native.atomic_plane_property_add_success_count = 0U;
    fake_result.native.atomic_commit_api_call_count = 0U;
    fake_result.native.atomic_request_free_call_count = 0U;
    fake_result.portable.outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_REJECTED;
    fake_result.portable.primary_outcome =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED;
    fake_result.portable.test_observation =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED;
    fake_result.portable.test_callback_reported_accepted_not_applied = false;
    fake_result.portable.test_error = EINVAL;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.native.atomic_commit_api_call_count = 0U;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.native.atomic_request_alloc_succeeded = false;
    fake_result.native.atomic_request_alloc_error = ENOMEM;
    fake_result.native.atomic_plane_property_add_attempt_count = 0U;
    fake_result.native.atomic_plane_property_add_success_count = 0U;
    fake_result.native.atomic_commit_api_call_count = 0U;
    fake_result.native.atomic_request_free_call_count = 0U;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.native.atomic_plane_property_add_attempt_count = 4U;
    fake_result.native.atomic_plane_property_add_success_count = 3U;
    fake_result.native.atomic_plane_property_add_error = ENOMEM;
    fake_result.native.atomic_commit_api_call_count = 0U;
    fake_result.native.atomic_request_free_call_count = 1U;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_result.portable.topology_read_attempt_count = 2U;
    fake_result.native.topology_read_call_count = 2U;
    fake_result.native.topology_read_attempted[2] = false;
    fake_result.native.topology_read_ready_for_evaluation[2] = false;
    fake_result.native.topology_read_topology_complete[2] = false;
    fake_result.native.topology_read_all_plane_properties_complete[2] = false;
    fake_result.native.topology_read_raw_connector_count[2] = 0U;
    fake_result.native.topology_read_raw_crtc_count[2] = 0U;
    fake_result.native.topology_read_raw_plane_count[2] = 0U;
    fake_result.native.topology_read_objects_seen[2] = 0U;
    fake_result.native.topology_read_objects_released[2] = 0U;
    fake_result.native.topology_read_objects_all_released[2] = false;
    CHECK(expect_provider_failure());

    reset_accepted_result();
    {
        unsigned char invalid_bool[sizeof(bool)];
        memset(invalid_bool, 0, sizeof(invalid_bool));
        invalid_bool[0] = 2U;
        memcpy(
            &fake_result.native.atomic_commit_user_data_null,
            invalid_bool, sizeof(invalid_bool)
        );
    }
    CHECK(expect_provider_failure());

    reset_accepted_result();
    fake_status = MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG;
    CHECK(expect_provider_failure());
    return true;
}

static bool test_argument_failures_and_stream_alias(void)
{
    static char hash[] =
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20";
    char *bad_arguments[] = {
        (char *)"probe",
        (char *)"--device", (char *)"relative/card0",
        (char *)"--topology-sha256", hash,
        (char *)"--connector-id", (char *)"10",
        (char *)"--crtc-id", (char *)"30",
        (char *)"--plane-id", (char *)"50"
    };
    char *good_arguments[] = {
        (char *)"probe",
        (char *)"--device", (char *)"/sensitive/card0",
        (char *)"--topology-sha256", hash,
        (char *)"--connector-id", (char *)"10",
        (char *)"--crtc-id", (char *)"30",
        (char *)"--plane-id", (char *)"50"
    };
    static const char STREAM_CANARY[] = "unchanged-stream";
    char output[128];
    char errors[128];
    FILE *out_stream = tmpfile();
    FILE *error_stream = tmpfile();
    FILE *aliased_stream = tmpfile();
    int return_code;

    CHECK(out_stream != NULL && error_stream != NULL &&
          aliased_stream != NULL);
    reset_accepted_result();
    return_code = meiso_k230d_drm_single_plane_test_only_once_run(
        out_stream, error_stream, 11, bad_arguments, fake_execute
    );
    CHECK(return_code == 2 && fake_calls == 0U);
    CHECK(read_stream(out_stream, output, sizeof(output)));
    CHECK(read_stream(error_stream, errors, sizeof(errors)));
    CHECK(output[0] == '\0' && strcmp(errors, "E_ARGUMENT\n") == 0);
    CHECK(fclose(out_stream) == 0 && fclose(error_stream) == 0);

    reset_accepted_result();
    CHECK(fwrite(
        STREAM_CANARY, 1U, sizeof(STREAM_CANARY) - 1U, aliased_stream
    ) == sizeof(STREAM_CANARY) - 1U);
    CHECK(fflush(aliased_stream) == 0);
    CHECK(ftell(aliased_stream) == (long)(sizeof(STREAM_CANARY) - 1U));
    return_code = meiso_k230d_drm_single_plane_test_only_once_run(
        aliased_stream, aliased_stream, 11, bad_arguments, fake_execute
    );
    CHECK(return_code == 2 && fake_calls == 0U);
    CHECK(ftell(aliased_stream) == (long)(sizeof(STREAM_CANARY) - 1U));
    CHECK(read_stream(aliased_stream, output, sizeof(output)));
    CHECK(strcmp(output, STREAM_CANARY) == 0);
    CHECK(fclose(aliased_stream) == 0);

    error_stream = tmpfile();
    CHECK(error_stream != NULL);
    CHECK(fwrite(
        STREAM_CANARY, 1U, sizeof(STREAM_CANARY) - 1U, error_stream
    ) == sizeof(STREAM_CANARY) - 1U);
    CHECK(fflush(error_stream) == 0);
    CHECK(ftell(error_stream) == (long)(sizeof(STREAM_CANARY) - 1U));
    CHECK(meiso_k230d_drm_single_plane_test_only_once_run(
              NULL, error_stream, 11, bad_arguments, fake_execute
          ) == 2);
    CHECK(ftell(error_stream) == (long)(sizeof(STREAM_CANARY) - 1U));
    CHECK(read_stream(error_stream, errors, sizeof(errors)));
    CHECK(strcmp(errors, STREAM_CANARY) == 0);
    CHECK(fclose(error_stream) == 0);

    out_stream = tmpfile();
    CHECK(out_stream != NULL);
    CHECK(fwrite(
        STREAM_CANARY, 1U, sizeof(STREAM_CANARY) - 1U, out_stream
    ) == sizeof(STREAM_CANARY) - 1U);
    CHECK(fflush(out_stream) == 0);
    CHECK(ftell(out_stream) == (long)(sizeof(STREAM_CANARY) - 1U));
    CHECK(meiso_k230d_drm_single_plane_test_only_once_run(
              out_stream, NULL, 11, bad_arguments, fake_execute
          ) == 2);
    CHECK(ftell(out_stream) == (long)(sizeof(STREAM_CANARY) - 1U));
    CHECK(read_stream(out_stream, output, sizeof(output)));
    CHECK(strcmp(output, STREAM_CANARY) == 0);
    CHECK(fclose(out_stream) == 0);

    out_stream = fopen("/dev/null", "r");
    error_stream = tmpfile();
    CHECK(out_stream != NULL && error_stream != NULL);
    reset_accepted_result();
    CHECK(meiso_k230d_drm_single_plane_test_only_once_run(
              out_stream, error_stream, 11, good_arguments, fake_execute
          ) == 1);
    CHECK(fake_calls == 1U);
    CHECK(read_stream(error_stream, errors, sizeof(errors)));
    CHECK(errors[0] == '\0');
    CHECK(fclose(out_stream) == 0 && fclose(error_stream) == 0);
    return true;
}

int main(void)
{
    if (!test_accepted_and_initial_blocked_receipts() ||
        !test_cleanup_failure_preserves_master_history() ||
        !test_valid_failure_receipts() ||
        !test_provider_contract_failures() ||
        !test_argument_failures_and_stream_alias()) {
        return 1;
    }
    (void)printf("K230D_DRM_SINGLE_PLANE_TEST_ONLY_RUNNER_TEST_PASSED\n");
    return 0;
}
