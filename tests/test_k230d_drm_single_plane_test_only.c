#include "meiso_drm_single_plane_test_only_private.h"

#include <errno.h>
#include <stddef.h>
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
    FAKE_DUMB_HANDLE = 70,
    FAKE_FRAMEBUFFER_ID = 80,
    FAKE_LOG_CAPACITY = 32
};

typedef uint8_t FakeOperation;
enum {
    FAKE_OPEN = 1,
    FAKE_ENABLE_UNIVERSAL = 2,
    FAKE_ENABLE_ATOMIC = 3,
    FAKE_ACQUIRE_MASTER = 4,
    FAKE_VERIFY_MASTER = 5,
    FAKE_READ_TOPOLOGY = 6,
    FAKE_CREATE_DUMB = 7,
    FAKE_ADD_FRAMEBUFFER = 8,
    FAKE_TEST_ONLY = 9,
    FAKE_REMOVE_FRAMEBUFFER = 10,
    FAKE_DESTROY_DUMB = 11,
    FAKE_DROP_MASTER = 12,
    FAKE_CLOSE = 13
};

typedef struct {
    MeisoK230dDrmTopology topologies[3];
    int topology_errors[3];
    bool topology_writes_on_failure[3];
    bool topology_omits_success_output[3];
    MeisoK230dDrmDumbBuffer dumb;
    uint32_t framebuffer_id;
    MeisoK230dDrmSinglePlaneTestOnlyCallDisposition test_disposition;
    MeisoK230dDrmAtomicPlaneState observed_state;
    int fd;
    int open_error;
    int universal_error;
    int atomic_error;
    int acquire_error;
    int verify_error;
    int create_error;
    int add_error;
    int test_error;
    int remove_error;
    int destroy_error;
    int drop_error;
    int close_error;
    bool open_writes_on_failure;
    bool create_writes_on_failure;
    bool add_writes_on_failure;
    bool mutate_atomic_state;
    uint32_t topology_cursor;
    uint32_t test_calls;
    uint32_t close_calls;
    FakeOperation log[FAKE_LOG_CAPACITY];
    uint32_t log_count;
} FakeDrm;

typedef struct {
    FakeDrm fake;
    MeisoK230dDrmSinglePlaneTestOnlyConfig config;
    MeisoK230dDrmSinglePlaneTestCandidateRequest request;
} Fixture;

static void log_operation(FakeDrm *fake, FakeOperation operation)
{
    if (fake->log_count < FAKE_LOG_CAPACITY) {
        fake->log[fake->log_count++] = operation;
    }
}

static int fake_open(void *context, const char *path, int *out_fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OPEN);
    if (fake->open_error != 0) {
        if (fake->open_writes_on_failure) {
            *out_fd = fake->fd;
        }
        return fake->open_error;
    }
    if (strcmp(path, "/dev/dri/card0") != 0) {
        return EINVAL;
    }
    *out_fd = fake->fd;
    return 0;
}

static int fake_universal(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_ENABLE_UNIVERSAL);
    return fd == fake->fd ? fake->universal_error : EBADF;
}

static int fake_atomic_capability(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_ENABLE_ATOMIC);
    return fd == fake->fd ? fake->atomic_error : EBADF;
}

static int fake_acquire(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_ACQUIRE_MASTER);
    return fd == fake->fd ? fake->acquire_error : EBADF;
}

static int fake_verify(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_VERIFY_MASTER);
    return fd == fake->fd ? fake->verify_error : EBADF;
}

static int fake_read(
    void *context,
    int fd,
    MeisoK230dDrmTopology *out_topology
)
{
    FakeDrm *fake = (FakeDrm *)context;
    uint32_t index = fake->topology_cursor;
    log_operation(fake, FAKE_READ_TOPOLOGY);
    if (fd != fake->fd || index >= 3U) {
        return EOVERFLOW;
    }
    ++fake->topology_cursor;
    if (fake->topology_errors[index] != 0) {
        if (fake->topology_writes_on_failure[index]) {
            *out_topology = fake->topologies[index];
        }
        return fake->topology_errors[index];
    }
    if (!fake->topology_omits_success_output[index]) {
        *out_topology = fake->topologies[index];
    }
    return 0;
}

static int fake_create(
    void *context,
    int fd,
    uint32_t width,
    uint32_t height,
    MeisoK230dDrmDumbBuffer *out_buffer
)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_CREATE_DUMB);
    if (fd != fake->fd || width != 4U || height != 4U) {
        return EINVAL;
    }
    if (fake->create_error != 0) {
        if (fake->create_writes_on_failure) {
            *out_buffer = fake->dumb;
        }
        return fake->create_error;
    }
    *out_buffer = fake->dumb;
    return 0;
}

static int fake_add(
    void *context,
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t handle,
    uint32_t pitch,
    uint32_t *out_framebuffer_id
)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_ADD_FRAMEBUFFER);
    if (fd != fake->fd || width != 4U || height != 4U ||
        handle != FAKE_DUMB_HANDLE || pitch != 8U) {
        return EINVAL;
    }
    if (fake->add_error != 0) {
        if (fake->add_writes_on_failure) {
            *out_framebuffer_id = fake->framebuffer_id;
        }
        return fake->add_error;
    }
    *out_framebuffer_id = fake->framebuffer_id;
    return 0;
}

static MeisoK230dDrmSinglePlaneTestOnlyCallDisposition fake_test_only(
    void *context,
    int fd,
    const MeisoK230dDrmAtomicPlaneState *state,
    int *out_error
)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_TEST_ONLY);
    if (fd != fake->fd) {
        *out_error = EBADF;
        return
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED;
    }
    ++fake->test_calls;
    fake->observed_state = *state;
    if (fake->mutate_atomic_state) {
        MeisoK230dDrmAtomicPlaneState *mutable_state =
            (MeisoK230dDrmAtomicPlaneState *)(void *)state;
        mutable_state->plane_id ^= UINT32_C(1);
    }
    *out_error = fake->test_error;
    return fake->test_disposition;
}

static int fake_remove(void *context, int fd, uint32_t framebuffer_id)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_REMOVE_FRAMEBUFFER);
    if (fd != fake->fd || framebuffer_id != fake->framebuffer_id) {
        return EINVAL;
    }
    return fake->remove_error;
}

static int fake_destroy(void *context, int fd, uint32_t handle)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_DESTROY_DUMB);
    if (fd != fake->fd || handle != FAKE_DUMB_HANDLE) {
        return EINVAL;
    }
    return fake->destroy_error;
}

static int fake_drop(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_DROP_MASTER);
    return fd == fake->fd ? fake->drop_error : EBADF;
}

static int fake_close(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_CLOSE);
    ++fake->close_calls;
    return fd == fake->fd ? fake->close_error : EBADF;
}

static const MeisoK230dDrmSinglePlaneTestOnlyOps FAKE_OPS = {
    fake_open,
    fake_universal,
    fake_atomic_capability,
    fake_acquire,
    fake_verify,
    fake_read,
    fake_create,
    fake_add,
    fake_test_only,
    fake_remove,
    fake_destroy,
    fake_drop,
    fake_close
};

static void fill_properties(MeisoK230dDrmPlane *plane)
{
    uint32_t index;
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        plane->properties[index].property_id = 100U + index;
        plane->properties[index].value = 0U;
    }
}

static MeisoK230dDrmTopology test_topology(void)
{
    MeisoK230dDrmTopology topology;
    memset(&topology, 0, sizeof(topology));
    memcpy(topology.driver_name, "fake-drm", sizeof("fake-drm"));
    topology.driver_version_major = 1U;
    topology.driver_version_minor = 2U;
    topology.driver_version_patch = 3U;
    topology.connector_count = 1U;
    topology.connectors[0].connector_id = 10U;
    topology.connectors[0].connector_type = 11U;
    topology.connectors[0].connector_type_id = 1U;
    topology.connectors[0].encoder_id = 20U;
    topology.connectors[0].crtc_id = 30U;
    topology.connectors[0].connected = true;
    topology.crtc_count = 1U;
    topology.crtcs[0].crtc_id = 30U;
    topology.crtcs[0].active = true;
    topology.crtcs[0].mode_valid = true;
    memcpy(topology.crtcs[0].mode.name, "4x4", sizeof("4x4"));
    topology.crtcs[0].mode.clock_khz = 1000U;
    topology.crtcs[0].mode.hdisplay = 4U;
    topology.crtcs[0].mode.hsync_start = 5U;
    topology.crtcs[0].mode.hsync_end = 6U;
    topology.crtcs[0].mode.htotal = 7U;
    topology.crtcs[0].mode.vdisplay = 4U;
    topology.crtcs[0].mode.vsync_start = 5U;
    topology.crtcs[0].mode.vsync_end = 6U;
    topology.crtcs[0].mode.vtotal = 7U;
    topology.plane_count = 1U;
    topology.planes[0].plane_id = 50U;
    topology.planes[0].possible_crtcs = 1U;
    topology.planes[0].type = MEISO_K230D_DRM_PLANE_TYPE_OVERLAY;
    topology.planes[0].type_immutable = true;
    topology.planes[0].supports_rgb565 = true;
    fill_properties(&topology.planes[0]);
    return topology;
}

static bool fixture_init(Fixture *fixture)
{
    uint32_t index;
    memset(fixture, 0, sizeof(*fixture));
    fixture->fake.fd = 0;
    fixture->fake.dumb.handle = FAKE_DUMB_HANDLE;
    fixture->fake.dumb.pitch = 8U;
    fixture->fake.dumb.size = 32U;
    fixture->fake.framebuffer_id = FAKE_FRAMEBUFFER_ID;
    fixture->fake.test_disposition =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED;
    for (index = 0U; index < 3U; ++index) {
        fixture->fake.topologies[index] = test_topology();
    }
    fixture->config.device_path = "/dev/dri/card0";
    fixture->config.ops = &FAKE_OPS;
    fixture->config.ops_context = &fixture->fake;
    CHECK(meiso_k230d_drm_private_topology_sha256(
        &fixture->fake.topologies[0],
        fixture->request.expected_topology_sha256
    ));
    fixture->request.connector_id = 10U;
    fixture->request.crtc_id = 30U;
    fixture->request.plane_id = 50U;
    return true;
}

static bool log_is(
    const FakeDrm *fake,
    const FakeOperation *expected,
    size_t expected_count
)
{
    return fake->log_count == expected_count &&
        memcmp(fake->log, expected,
               expected_count * sizeof(expected[0])) == 0;
}

static bool test_accepted_fd_zero_and_three_reads(void)
{
    static const FakeOperation expected[] = {
        FAKE_OPEN, FAKE_ENABLE_UNIVERSAL, FAKE_ENABLE_ATOMIC,
        FAKE_ACQUIRE_MASTER, FAKE_VERIFY_MASTER, FAKE_READ_TOPOLOGY,
        FAKE_CREATE_DUMB, FAKE_ADD_FRAMEBUFFER, FAKE_READ_TOPOLOGY,
        FAKE_TEST_ONLY, FAKE_READ_TOPOLOGY, FAKE_REMOVE_FRAMEBUFFER,
        FAKE_DESTROY_DUMB, FAKE_DROP_MASTER, FAKE_CLOSE
    };
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;
    static const uint64_t expected_values[
        MEISO_K230D_DRM_PLANE_PROPERTY_COUNT
    ] = {
        FAKE_FRAMEBUFFER_ID, 30U, 0U, 0U,
        UINT64_C(4) << 16U, UINT64_C(4) << 16U,
        0U, 0U, 4U, 4U
    };
    uint32_t property_index;

    CHECK(fixture_init(&fixture));
    memset(&result, 0xa5, sizeof(result));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.test_observation ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE);
    CHECK(result.postcondition_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_NONE);
    CHECK(result.cleanup_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_NONE);
    CHECK(result.topology_read_attempt_count == 3U &&
          result.topology_read_success_count == 3U &&
          result.atomic_test_only_call_count == 1U);
    CHECK(result.initial_candidate_identified &&
          result.pretest_candidate_identified &&
          result.posttest_candidate_identified &&
          result.pretest_topology_unchanged &&
          result.posttest_topology_unchanged &&
          result.test_only_attempted &&
          result.test_callback_reported_accepted_not_applied);
    CHECK(result.fd_was_opened && result.universal_planes_enabled &&
          result.atomic_enabled && result.master_was_acquired &&
          result.master_verified && result.dumb_was_created &&
          result.framebuffer_was_created);
    CHECK(result.created_dumb.handle == FAKE_DUMB_HANDLE &&
          result.created_dumb.pitch == 8U &&
          result.created_dumb.size == 32U &&
          result.created_framebuffer_id == FAKE_FRAMEBUFFER_ID);
    CHECK(result.remove_framebuffer_succeeded &&
          result.destroy_dumb_succeeded && result.drop_master_succeeded &&
          result.close_succeeded && result.fd_ownership_consumed &&
          result.master_ownership_consumed &&
          result.dumb_ownership_consumed &&
          result.framebuffer_ownership_consumed);
    CHECK(!result.pixel_payload_present && fixture.fake.close_calls == 1U);
    CHECK(log_is(&fixture.fake, expected,
                 sizeof(expected) / sizeof(expected[0])));
    CHECK(fixture.fake.observed_state.plane_id == 50U);
    for (property_index = 0U;
         property_index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++property_index) {
        CHECK(fixture.fake.observed_state.properties[property_index].
                  property_id == 100U + property_index);
        CHECK(fixture.fake.observed_state.properties[property_index].value ==
              expected_values[property_index]);
    }
    return true;
}

static bool test_cleanup_failures_continue_to_terminal_close(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.remove_error = EBUSY;
    fixture.fake.destroy_error = EIO;
    fixture.fake.drop_error = EPERM;
    fixture.fake.close_error = EINTR;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.cleanup_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER);
    CHECK(result.cleanup_error == EBUSY);
    CHECK(result.remove_framebuffer_attempted &&
          result.destroy_dumb_attempted && result.drop_master_attempted &&
          result.close_attempted);
    CHECK(!result.remove_framebuffer_succeeded &&
          !result.destroy_dumb_succeeded && !result.drop_master_succeeded &&
          !result.close_succeeded);
    CHECK(result.fd_ownership_consumed &&
          result.master_ownership_consumed &&
          result.dumb_ownership_consumed &&
          result.framebuffer_ownership_consumed);
    CHECK(fixture.fake.close_calls == 1U);
    return true;
}

static bool test_failed_open_output_is_not_owned(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.open_error = EIO;
    fixture.fake.open_writes_on_failure = true;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY);
    CHECK(result.operation_error == EPROTO);
    CHECK(!result.fd_was_opened && !result.close_attempted &&
          !result.fd_ownership_consumed);
    CHECK(fixture.fake.log_count == 1U &&
          fixture.fake.log[0] == FAKE_OPEN && fixture.fake.close_calls == 0U);
    return true;
}

static bool test_topology_failure_write_is_protocol_error(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.topology_errors[0] = EIO;
    fixture.fake.topology_writes_on_failure[0] = true;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY);
    CHECK(result.operation_error == EPROTO);
    CHECK(result.topology_read_attempt_count == 1U &&
          result.topology_read_success_count == 0U &&
          result.atomic_test_only_call_count == 0U);
    CHECK(result.drop_master_succeeded && result.close_succeeded &&
          fixture.fake.close_calls == 1U);
    return true;
}

static bool test_pretest_block_and_posttest_failure(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.topologies[1].driver_version_patch = 4U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE);
    CHECK(result.operation_error == 0 &&
          result.pretest_candidate_reason ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_IDENTITY_MISMATCH);
    CHECK(result.topology_read_attempt_count == 2U &&
          result.atomic_test_only_call_count == 0U &&
          fixture.fake.test_calls == 0U);

    CHECK(fixture_init(&fixture));
    fixture.fake.topology_errors[2] = EIO;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.test_observation ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.postcondition_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY);
    CHECK(result.postcondition_error == EIO &&
          result.atomic_test_only_call_count == 1U &&
          result.topology_read_attempt_count == 3U);
    CHECK(result.close_succeeded && fixture.fake.close_calls == 1U);
    return true;
}

static bool test_invalid_invocation_preserves_output(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;
    uint8_t before[sizeof(result)];

    CHECK(fixture_init(&fixture));
    memset(&result, 0xa5, sizeof(result));
    memcpy(before, &result, sizeof(before));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              NULL, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT);
    CHECK(memcmp(&result, before, sizeof(result)) == 0 &&
          fixture.fake.log_count == 0U);
    fixture.request.plane_id = 0U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT);
    CHECK(memcmp(&result, before, sizeof(result)) == 0 &&
          fixture.fake.log_count == 0U);
    return true;
}

static bool test_initial_block_is_clean_not_tested(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.topologies[0].planes[0].supports_rgb565 = false;
    CHECK(meiso_k230d_drm_private_topology_sha256(
        &fixture.fake.topologies[0],
        fixture.request.expected_topology_sha256
    ));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_NOT_TESTED);
    CHECK(result.initial_candidate_reason ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_NO_TEST_TARGET);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE);
    CHECK(result.operation_error == 0 &&
          result.test_observation ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_NOT_ATTEMPTED &&
          result.atomic_test_only_call_count == 0U &&
          result.topology_read_attempt_count == 1U);
    CHECK(result.drop_master_succeeded && result.close_succeeded &&
          fixture.fake.close_calls == 1U);
    return true;
}

static bool test_atomic_disposition_and_protocol_matrix(void)
{
    typedef struct {
        MeisoK230dDrmSinglePlaneTestOnlyCallDisposition disposition;
        int callback_error;
        bool mutate_state;
        MeisoK230dDrmSinglePlaneTestOnlyOutcome outcome;
        MeisoK230dDrmSinglePlaneTestOnlyPrimaryOutcome primary;
        MeisoK230dDrmSinglePlaneTestOnlyTestObservation observation;
        int recorded_error;
    } Case;
    static const Case cases[] = {
        {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_REJECTED_NOT_APPLIED,
            EACCES, false,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_TEST_REJECTED,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED,
            EACCES
        },
        {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED,
            EIO, false,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED,
            EIO
        },
        {
            (MeisoK230dDrmSinglePlaneTestOnlyCallDisposition)99U,
            0, false,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED,
            EPROTO
        },
        {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED,
            EIO, false,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED,
            EPROTO
        },
        {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_REJECTED_NOT_APPLIED,
            0, false,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED,
            EPROTO
        },
        {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED,
            0, false,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED,
            EPROTO
        },
        {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED,
            -EIO, false,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED,
            EPROTO
        },
        {
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED,
            0, true,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE,
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_OPERATION_FAILED_NOT_APPLIED,
            EPROTO
        }
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        Fixture fixture;
        MeisoK230dDrmSinglePlaneTestOnlyResult result;
        CHECK(fixture_init(&fixture));
        fixture.fake.test_disposition = cases[index].disposition;
        fixture.fake.test_error = cases[index].callback_error;
        fixture.fake.mutate_atomic_state = cases[index].mutate_state;
        CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
                  &fixture.config, &fixture.request, &result
              ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
        CHECK(result.outcome == cases[index].outcome &&
              result.primary_outcome == cases[index].primary &&
              result.test_observation == cases[index].observation &&
              result.test_error == cases[index].recorded_error);
        if (cases[index].outcome ==
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE) {
            CHECK(result.operation_failure_stage ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY);
            CHECK(result.operation_error == cases[index].recorded_error);
        } else {
            CHECK(result.operation_failure_stage ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE);
            CHECK(result.operation_error == 0);
        }
        CHECK(result.atomic_test_only_call_count == 1U &&
              result.topology_read_attempt_count == 3U &&
              result.topology_read_success_count == 3U &&
              result.posttest_topology_unchanged &&
              result.close_succeeded && fixture.fake.close_calls == 1U);
    }
    return true;
}

static bool test_posttest_drift_and_failure_write(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.topologies[2].driver_version_patch = 4U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.test_observation ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_ACCEPTED_NOT_APPLIED);
    CHECK(result.postcondition_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED);
    CHECK(result.postcondition_error == ESTALE &&
          result.posttest_candidate_reason ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_IDENTITY_MISMATCH);
    CHECK(result.close_succeeded && fixture.fake.close_calls == 1U);

    CHECK(fixture_init(&fixture));
    fixture.fake.topology_errors[2] = EIO;
    fixture.fake.topology_writes_on_failure[2] = true;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.postcondition_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY);
    CHECK(result.postcondition_error == EPROTO &&
          result.primary_outcome ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED &&
          result.close_succeeded);
    return true;
}

typedef uint8_t FailurePoint;
enum {
    FAIL_OPEN = 1,
    FAIL_UNIVERSAL = 2,
    FAIL_ATOMIC_CAPABILITY = 3,
    FAIL_ACQUIRE = 4,
    FAIL_VERIFY = 5,
    FAIL_INITIAL_READ = 6,
    FAIL_CREATE = 7,
    FAIL_ADD = 8,
    FAIL_PRETEST_READ = 9
};

static void set_operation_failure(
    FakeDrm *fake,
    FailurePoint point,
    int error
)
{
    switch (point) {
        case FAIL_OPEN:
            fake->open_error = error;
            break;
        case FAIL_UNIVERSAL:
            fake->universal_error = error;
            break;
        case FAIL_ATOMIC_CAPABILITY:
            fake->atomic_error = error;
            break;
        case FAIL_ACQUIRE:
            fake->acquire_error = error;
            break;
        case FAIL_VERIFY:
            fake->verify_error = error;
            break;
        case FAIL_INITIAL_READ:
            fake->topology_errors[0] = error;
            break;
        case FAIL_CREATE:
            fake->create_error = error;
            break;
        case FAIL_ADD:
            fake->add_error = error;
            break;
        case FAIL_PRETEST_READ:
            fake->topology_errors[1] = error;
            break;
        default:
            break;
    }
}

static MeisoK230dDrmSinglePlaneTestOnlyOperationStage stage_for_failure(
    FailurePoint point
)
{
    static const MeisoK230dDrmSinglePlaneTestOnlyOperationStage stages[] = {
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_UNIVERSAL_PLANES,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ENABLE_ATOMIC,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ACQUIRE_MASTER,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_INITIAL_TOPOLOGY,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_READ_PRETEST_TOPOLOGY
    };
    return stages[point];
}

static bool test_each_operation_stage_positive_and_negative_error(void)
{
    FailurePoint point;
    uint32_t sign_case;

    for (point = FAIL_OPEN; point <= FAIL_PRETEST_READ; ++point) {
        for (sign_case = 0U; sign_case < 2U; ++sign_case) {
            Fixture fixture;
            MeisoK230dDrmSinglePlaneTestOnlyResult result;
            const int callback_error = sign_case == 0U ? EIO : -EIO;
            const int expected_error = sign_case == 0U ? EIO : EPROTO;
            CHECK(fixture_init(&fixture));
            set_operation_failure(&fixture.fake, point, callback_error);
            CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
                      &fixture.config, &fixture.request, &result
                  ) ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
            CHECK(result.outcome ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
            CHECK(result.primary_outcome ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE);
            CHECK(result.operation_failure_stage == stage_for_failure(point));
            CHECK(result.operation_error == expected_error &&
                  result.atomic_test_only_call_count == 0U);
            if (point == FAIL_OPEN) {
                CHECK(!result.fd_was_opened && !result.close_attempted &&
                      fixture.fake.close_calls == 0U);
            } else {
                CHECK(result.fd_was_opened && result.close_attempted &&
                      result.close_succeeded &&
                      result.fd_ownership_consumed &&
                      fixture.fake.close_calls == 1U);
            }
        }
    }
    return true;
}

static bool test_create_and_add_output_contracts(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.create_error = EIO;
    fixture.fake.create_writes_on_failure = true;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB);
    CHECK(result.operation_error == EPROTO && !result.dumb_was_created &&
          !result.destroy_dumb_attempted && result.drop_master_succeeded &&
          result.close_succeeded);

    CHECK(fixture_init(&fixture));
    fixture.fake.dumb.handle = 0U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_CREATE_DUMB);
    CHECK(result.operation_error == EPROTO && !result.dumb_was_created &&
          !result.destroy_dumb_attempted && result.close_succeeded);

    CHECK(fixture_init(&fixture));
    fixture.fake.dumb.pitch = 7U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB);
    CHECK(result.operation_error == EPROTO && result.dumb_was_created &&
          result.destroy_dumb_succeeded && result.close_succeeded);

    CHECK(fixture_init(&fixture));
    fixture.fake.dumb.size = 31U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB);
    CHECK(result.dumb_was_created && result.destroy_dumb_succeeded);

    CHECK(fixture_init(&fixture));
    fixture.fake.dumb.size = MEISO_K230D_DRM_MAX_DUMB_BYTES + UINT64_C(1);
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VALIDATE_DUMB);
    CHECK(result.dumb_was_created && result.destroy_dumb_succeeded);

    CHECK(fixture_init(&fixture));
    fixture.fake.add_error = EIO;
    fixture.fake.add_writes_on_failure = true;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER);
    CHECK(result.operation_error == EPROTO &&
          !result.framebuffer_was_created &&
          !result.remove_framebuffer_attempted &&
          result.destroy_dumb_succeeded && result.close_succeeded);

    CHECK(fixture_init(&fixture));
    fixture.fake.framebuffer_id = 0U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ADD_FRAMEBUFFER);
    CHECK(result.operation_error == EPROTO &&
          !result.framebuffer_was_created &&
          !result.remove_framebuffer_attempted &&
          result.destroy_dumb_succeeded && result.close_succeeded);
    return true;
}

static bool test_cleanup_stage_matrix_and_primary_preservation(void)
{
    MeisoK230dDrmSinglePlaneTestOnlyCleanupStage stage;
    uint32_t sign_case;

    for (stage =
             MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER;
         stage <= MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_CLOSE_DEVICE;
         ++stage) {
        for (sign_case = 0U; sign_case < 2U; ++sign_case) {
            Fixture fixture;
            MeisoK230dDrmSinglePlaneTestOnlyResult result;
            const int callback_error = sign_case == 0U ? EBUSY : -EBUSY;
            const int expected_error = sign_case == 0U ? EBUSY : EPROTO;
            CHECK(fixture_init(&fixture));
            if (stage ==
                MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_REMOVE_FRAMEBUFFER) {
                fixture.fake.remove_error = callback_error;
            } else if (stage ==
                       MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DESTROY_DUMB) {
                fixture.fake.destroy_error = callback_error;
            } else if (stage ==
                       MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER) {
                fixture.fake.drop_error = callback_error;
            } else {
                fixture.fake.close_error = callback_error;
            }
            CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
                      &fixture.config, &fixture.request, &result
                  ) ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
            CHECK(result.outcome ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE);
            CHECK(result.primary_outcome ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_ACCEPTED_NOT_APPLIED);
            CHECK(result.cleanup_failure_stage == stage &&
                  result.cleanup_error == expected_error);
            CHECK(result.remove_framebuffer_attempted &&
                  result.destroy_dumb_attempted &&
                  result.drop_master_attempted && result.close_attempted &&
                  fixture.fake.close_calls == 1U);
            CHECK(result.fd_ownership_consumed &&
                  result.master_ownership_consumed &&
                  result.dumb_ownership_consumed &&
                  result.framebuffer_ownership_consumed);
        }
    }

    {
        Fixture fixture;
        MeisoK230dDrmSinglePlaneTestOnlyResult result;
        CHECK(fixture_init(&fixture));
        fixture.fake.verify_error = EIO;
        fixture.fake.drop_error = EBUSY;
        CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
                  &fixture.config, &fixture.request, &result
              ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
        CHECK(result.outcome ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_CLEANUP_FAILURE);
        CHECK(result.primary_outcome ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE);
        CHECK(result.operation_failure_stage ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_VERIFY_MASTER);
        CHECK(result.operation_error == EIO &&
              result.cleanup_failure_stage ==
                  MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CLEANUP_DROP_MASTER &&
              result.cleanup_error == EBUSY && result.close_attempted &&
              result.fd_ownership_consumed);
    }
    return true;
}

static bool expect_no_callback_rejection(
    Fixture *fixture,
    const MeisoK230dDrmSinglePlaneTestOnlyConfig *config,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus expected_status
)
{
    MeisoK230dDrmSinglePlaneTestOnlyResult result;
    uint8_t before[sizeof(result)];
    memset(&result, 0xa5, sizeof(result));
    memcpy(before, &result, sizeof(before));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              config, request, &result
          ) == expected_status);
    CHECK(memcmp(&result, before, sizeof(result)) == 0 &&
          fixture->fake.log_count == 0U);
    return true;
}

static bool expect_existing_output_rejection(
    Fixture *fixture,
    const MeisoK230dDrmSinglePlaneTestOnlyConfig *config,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmSinglePlaneTestOnlyResult *out_result,
    MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus expected_status
)
{
    uint8_t before[sizeof(*out_result)];
    memcpy(before, out_result, sizeof(before));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              config, request, out_result
          ) == expected_status);
    CHECK(memcmp(out_result, before, sizeof(before)) == 0 &&
          fixture->fake.log_count == 0U);
    return true;
}

static bool test_invocation_null_zero_and_mixed_precedence(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyConfig config;
    MeisoK230dDrmSinglePlaneTestCandidateRequest request;

    CHECK(fixture_init(&fixture));
    CHECK(expect_no_callback_rejection(
        &fixture, NULL, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT
    ));
    CHECK(expect_no_callback_rejection(
        &fixture, &fixture.config, NULL,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT
    ));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, NULL
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT);
    CHECK(fixture.fake.log_count == 0U);

    request = fixture.request;
    request.connector_id = 0U;
    CHECK(expect_no_callback_rejection(
        &fixture, &fixture.config, &request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT
    ));
    request = fixture.request;
    request.crtc_id = 0U;
    CHECK(expect_no_callback_rejection(
        &fixture, &fixture.config, &request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT
    ));
    request = fixture.request;
    request.plane_id = 0U;
    CHECK(expect_no_callback_rejection(
        &fixture, &fixture.config, &request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT
    ));

    config = fixture.config;
    config.device_path = NULL;
    request = fixture.request;
    request.plane_id = 0U;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT
    ));
    return true;
}

static bool test_invocation_fixed_object_aliases(void)
{
    union {
        max_align_t alignment;
        MeisoK230dDrmSinglePlaneTestOnlyConfig config;
        MeisoK230dDrmSinglePlaneTestCandidateRequest request;
        MeisoK230dDrmSinglePlaneTestOnlyResult result;
    } shared;
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;
    uint8_t before[sizeof(shared)];

    CHECK(fixture_init(&fixture));
    memset(&shared, 0, sizeof(shared));
    shared.config = fixture.config;
    memset(&result, 0xa5, sizeof(result));
    memcpy(before, &shared, sizeof(shared));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &shared.config,
              (const MeisoK230dDrmSinglePlaneTestCandidateRequest *)(
                  const void *)&shared,
              &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT);
    CHECK(memcmp(&shared, before, sizeof(shared)) == 0 &&
          fixture.fake.log_count == 0U);

    memset(&shared, 0, sizeof(shared));
    shared.config = fixture.config;
    memcpy(before, &shared, sizeof(shared));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &shared.config, &fixture.request,
              (MeisoK230dDrmSinglePlaneTestOnlyResult *)(void *)&shared
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT);
    CHECK(memcmp(&shared, before, sizeof(shared)) == 0 &&
          fixture.fake.log_count == 0U);

    memset(&shared, 0, sizeof(shared));
    shared.request = fixture.request;
    memcpy(before, &shared, sizeof(shared));
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &shared.request,
              (MeisoK230dDrmSinglePlaneTestOnlyResult *)(void *)&shared
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT);
    CHECK(memcmp(&shared, before, sizeof(shared)) == 0 &&
          fixture.fake.log_count == 0U);
    return true;
}

static bool test_invocation_configuration_matrix(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyConfig config;
    MeisoK230dDrmSinglePlaneTestOnlyOps ops;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;
    char long_path[MEISO_K230D_DRM_DEVICE_PATH_CAPACITY];

    CHECK(fixture_init(&fixture));
    config = fixture.config;
    config.device_path = NULL;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops = NULL;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops_context = NULL;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.device_path = "relative";
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    memset(long_path, 'a', sizeof(long_path));
    long_path[0] = '/';
    config = fixture.config;
    config.device_path = long_path;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    ops = FAKE_OPS;
    ops.atomic_test_only = NULL;
    config = fixture.config;
    config.ops = &ops;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));

    memset(&result, 0xa5, sizeof(result));
    config = fixture.config;
    config.device_path = (const char *)(const void *)&config;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.device_path =
        (const char *)(const void *)&fixture.request;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.device_path = (const char *)(const void *)&FAKE_OPS;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.device_path = (const char *)(const void *)&result;
    CHECK(expect_existing_output_rejection(
        &fixture, &config, &fixture.request, &result,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));

    config = fixture.config;
    config.ops = (const MeisoK230dDrmSinglePlaneTestOnlyOps *)(
        const void *)&config;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops = (const MeisoK230dDrmSinglePlaneTestOnlyOps *)(
        const void *)&fixture.request;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops = (const MeisoK230dDrmSinglePlaneTestOnlyOps *)(
        const void *)&result;
    CHECK(expect_existing_output_rejection(
        &fixture, &config, &fixture.request, &result,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));

    config = fixture.config;
    config.ops_context = &config;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops_context = &fixture.request;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops_context = &result;
    CHECK(expect_existing_output_rejection(
        &fixture, &config, &fixture.request, &result,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops_context = (void *)(uintptr_t)(const void *)&FAKE_OPS;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    config = fixture.config;
    config.ops_context = (void *)(uintptr_t)(const void *)config.device_path;
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    return true;
}

static bool test_success_output_sentinels_and_post_axes(void)
{
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyResult result;

    CHECK(fixture_init(&fixture));
    fixture.fake.fd = -1;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_OPEN_PRIMARY);
    CHECK(result.operation_error == EPROTO && !result.fd_was_opened &&
          !result.close_attempted && fixture.fake.close_calls == 0U);

    CHECK(fixture_init(&fixture));
    fixture.fake.topology_omits_success_output[0] = true;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_NOT_TESTED);
    CHECK(result.initial_candidate_reason ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_CANDIDATE_REASON_TOPOLOGY_NONCANONICAL);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_NONE);
    CHECK(result.close_succeeded && fixture.fake.close_calls == 1U);

    CHECK(fixture_init(&fixture));
    fixture.fake.test_disposition =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_REJECTED_NOT_APPLIED;
    fixture.fake.test_error = EACCES;
    fixture.fake.topologies[2].driver_version_patch = 4U;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_TEST_REJECTED);
    CHECK(result.test_observation ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_TEST_REJECTED_NOT_APPLIED);
    CHECK(result.test_error == EACCES &&
          result.postcondition_failure_stage ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_TOPOLOGY_CHANGED &&
          result.postcondition_error == ESTALE && result.close_succeeded);

    CHECK(fixture_init(&fixture));
    fixture.fake.test_disposition =
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED;
    fixture.fake.test_error = EIO;
    fixture.fake.topology_errors[2] = ENODEV;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &fixture.config, &fixture.request, &result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE);
    CHECK(result.outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OUTCOME_OPERATION_FAILURE);
    CHECK(result.primary_outcome ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_PRIMARY_OPERATION_FAILURE);
    CHECK(result.operation_failure_stage ==
          MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_OPERATION_ATOMIC_TEST_ONLY);
    CHECK(result.operation_error == EIO &&
          result.postcondition_failure_stage ==
              MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_POSTCONDITION_READ_TOPOLOGY &&
          result.postcondition_error == ENODEV && result.close_succeeded);
    return true;
}

static bool test_path_and_ops_overlap_beyond_base(void)
{
    union {
        max_align_t alignment;
        uint8_t bytes[
            sizeof(MeisoK230dDrmSinglePlaneTestOnlyResult) +
            sizeof(max_align_t) + 32U
        ];
    } storage;
    Fixture fixture;
    MeisoK230dDrmSinglePlaneTestOnlyConfig config;
    MeisoK230dDrmSinglePlaneTestOnlyResult *overlapped_result;
    MeisoK230dDrmSinglePlaneTestOnlyResult separate_result;
    uint8_t before[sizeof(*overlapped_result)];
    const size_t offset = sizeof(max_align_t);

    CHECK(fixture_init(&fixture));
    memset(&storage, 'p', sizeof(storage));
    storage.bytes[0] = '/';
    storage.bytes[offset + 8U] = '\0';
    overlapped_result =
        (MeisoK230dDrmSinglePlaneTestOnlyResult *)(void *)(
            storage.bytes + offset
        );
    memcpy(before, overlapped_result, sizeof(before));
    config = fixture.config;
    config.device_path = (const char *)(const void *)storage.bytes;
    CHECK(meiso_k230d_drm_private_run_single_plane_test_only(
              &config, &fixture.request, overlapped_result
          ) == MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG);
    CHECK(memcmp(overlapped_result, before, sizeof(before)) == 0 &&
          fixture.fake.log_count == 0U);

    memset(&storage, 'q', sizeof(storage));
    storage.bytes[0] = '/';
    storage.bytes[offset + 8U] = '\0';
    config = fixture.config;
    config.device_path = (const char *)(const void *)storage.bytes;
    config.ops = (const MeisoK230dDrmSinglePlaneTestOnlyOps *)(const void *)(
        storage.bytes + offset
    );
    memset(&separate_result, 0xa5, sizeof(separate_result));
    CHECK(expect_existing_output_rejection(
        &fixture, &config, &fixture.request, &separate_result,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));

    config = fixture.config;
    config.ops_context = (void *)(uintptr_t)(const void *)(
        config.device_path + 2
    );
    CHECK(expect_no_callback_rejection(
        &fixture, &config, &fixture.request,
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_CONFIG
    ));
    return true;
}

int main(void)
{
    if (!test_accepted_fd_zero_and_three_reads() ||
        !test_cleanup_failures_continue_to_terminal_close() ||
        !test_failed_open_output_is_not_owned() ||
        !test_topology_failure_write_is_protocol_error() ||
        !test_pretest_block_and_posttest_failure() ||
        !test_invalid_invocation_preserves_output() ||
        !test_initial_block_is_clean_not_tested() ||
        !test_atomic_disposition_and_protocol_matrix() ||
        !test_posttest_drift_and_failure_write() ||
        !test_each_operation_stage_positive_and_negative_error() ||
        !test_create_and_add_output_contracts() ||
        !test_cleanup_stage_matrix_and_primary_preservation() ||
        !test_invocation_null_zero_and_mixed_precedence() ||
        !test_invocation_fixed_object_aliases() ||
        !test_invocation_configuration_matrix() ||
        !test_success_output_sentinels_and_post_axes() ||
        !test_path_and_ops_overlap_beyond_base()) {
        return 1;
    }
    (void)printf("K230D_DRM_SINGLE_PLANE_TEST_ONLY_CORE_TEST_PASSED\n");
    return 0;
}
