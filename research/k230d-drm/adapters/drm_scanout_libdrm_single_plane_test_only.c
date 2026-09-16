#define _POSIX_C_SOURCE 200809L

#include "meiso_drm_libdrm_single_plane_test_only_private.h"

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

_Static_assert(
    MEISO_K230D_DRM_RGB565_FOURCC == DRM_FORMAT_RGB565,
    "Meiso RGB565 fourcc must match libdrm"
);
_Static_assert(
    MEISO_K230D_DRM_NATIVE_TEST_ONLY_FLAG_VALUE ==
        DRM_MODE_ATOMIC_TEST_ONLY,
    "Meiso native TEST_ONLY flag value must match libdrm"
);

typedef struct {
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyLedger ledger;
    int owned_fd;
    bool owned_fd_active;
} NativeTestOnlyContext;

static int positive_error(int result)
{
    if (errno > 0) {
        return errno;
    }
    if (result < -1 && result != INT_MIN) {
        return -result;
    }
    return EIO;
}

static int check_same_fd(NativeTestOnlyContext *context, int fd)
{
    if (context == NULL) {
        return EPROTO;
    }
    ++context->ledger.same_fd_check_count;
    if (!context->ledger.same_fd_contract_applicable ||
        !context->owned_fd_active || fd != context->owned_fd) {
        context->ledger.same_fd_contract_verified = false;
        return EPROTO;
    }
    return 0;
}

static int terminal_close_untransferred(
    NativeTestOnlyContext *context,
    int fd
)
{
    int result;
    int error = 0;

    ++context->ledger.open_failure_close_call_count;
    errno = 0;
    result = close(fd);
    context->ledger.open_failure_close_terminal_consumed = true;
    if (result != 0) {
        error = positive_error(result);
    }
    context->ledger.open_failure_close_error = error;
    return error;
}

static int native_open_primary(
    void *opaque,
    const char *path,
    int *out_fd
)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    struct stat descriptor_stat;
    int fd;
    int result;
    int error;

    if (context == NULL || path == NULL || out_fd == NULL) {
        return EPROTO;
    }
    ++context->ledger.open_primary_call_count;
    errno = 0;
    fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        error = positive_error(fd);
        context->ledger.open_primary_error = error;
        return error;
    }
    context->ledger.native_fd_opened = true;

    ++context->ledger.fstat_call_count;
    errno = 0;
    result = fstat(fd, &descriptor_stat);
    if (result != 0) {
        error = positive_error(result);
        context->ledger.open_primary_error = error;
        (void)terminal_close_untransferred(context, fd);
        return error;
    }
    if (!S_ISCHR(descriptor_stat.st_mode)) {
        context->ledger.open_primary_error = ENODEV;
        (void)terminal_close_untransferred(context, fd);
        return ENODEV;
    }

    ++context->ledger.primary_node_type_call_count;
    errno = 0;
    result = drmGetNodeTypeFromFd(fd);
    if (result != DRM_NODE_PRIMARY) {
        error = result < 0 ? positive_error(result) : ENODEV;
        context->ledger.open_primary_error = error;
        (void)terminal_close_untransferred(context, fd);
        return error;
    }
    context->ledger.primary_node_verified = true;
    context->owned_fd = fd;
    context->owned_fd_active = true;
    context->ledger.same_fd_contract_applicable = true;
    context->ledger.same_fd_contract_verified = true;
    *out_fd = fd;
    return 0;
}

static int native_enable_universal_planes(void *opaque, int fd)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    int result;

    if (check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    ++context->ledger.universal_planes_cap_call_count;
    errno = 0;
    result = drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1U);
    if (result != 0) {
        context->ledger.universal_planes_cap_error = positive_error(result);
        return context->ledger.universal_planes_cap_error;
    }
    context->ledger.universal_planes_cap_set = true;
    return 0;
}

static int native_enable_atomic(void *opaque, int fd)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    int result;

    if (check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    ++context->ledger.atomic_cap_call_count;
    errno = 0;
    result = drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1U);
    if (result != 0) {
        context->ledger.atomic_cap_error = positive_error(result);
        return context->ledger.atomic_cap_error;
    }
    context->ledger.atomic_cap_set = true;
    return 0;
}

static int native_acquire_master(void *opaque, int fd)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    int result;

    if (check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    ++context->ledger.set_master_call_count;
    errno = 0;
    result = drmSetMaster(fd);
    if (result != 0) {
        context->ledger.set_master_error = positive_error(result);
        return context->ledger.set_master_error;
    }
    context->ledger.master_was_acquired = true;
    return 0;
}

static int native_verify_master(void *opaque, int fd)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    int result;

    if (check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    ++context->ledger.is_master_call_count;
    errno = 0;
    result = drmIsMaster(fd);
    if (result < 0) {
        context->ledger.is_master_error = positive_error(result);
        return context->ledger.is_master_error;
    }
    if (result == 0) {
        context->ledger.is_master_error = EPERM;
        return EPERM;
    }
    context->ledger.master_verified = true;
    return 0;
}

static int native_read_topology(
    void *opaque,
    int fd,
    MeisoK230dDrmTopology *out_topology
)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    MeisoK230dDrmNativeTopologyFdRead read;
    MeisoK230dDrmNativeReadStatus status;
    uint32_t slot;

    if (out_topology == NULL || check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    slot = context->ledger.topology_read_call_count;
    if (slot >= MEISO_K230D_DRM_NATIVE_TEST_ONLY_TOPOLOGY_READ_COUNT) {
        return EPROTO;
    }
    ++context->ledger.topology_read_call_count;
    context->ledger.topology_read_attempted[slot] = true;
    memset(&read, 0, sizeof(read));
    status = meiso_k230d_drm_libdrm_read_topology_fd(fd, &read);
    context->ledger.topology_read_status[slot] = status;
    context->ledger.topology_read_stage[slot] = read.operation_stage;
    context->ledger.topology_read_preclassified_reason[slot] =
        read.preclassified_reason;
    context->ledger.topology_read_error[slot] = read.error;
    context->ledger.topology_read_ready_for_evaluation[slot] =
        read.topology_ready_for_evaluation;
    context->ledger.topology_read_topology_complete[slot] =
        read.topology_complete;
    context->ledger.topology_read_all_plane_properties_complete[slot] =
        read.all_plane_properties_complete;
    context->ledger.topology_read_raw_connector_count[slot] =
        read.raw_connector_count;
    context->ledger.topology_read_raw_crtc_count[slot] =
        read.raw_crtc_count;
    context->ledger.topology_read_raw_plane_count[slot] =
        read.raw_plane_count;
    context->ledger.topology_read_objects_seen[slot] =
        read.libdrm_objects_seen;
    context->ledger.topology_read_objects_released[slot] =
        read.libdrm_objects_released_count;
    context->ledger.topology_read_objects_all_released[slot] =
        read.libdrm_objects_released;

    if (status == MEISO_K230D_DRM_NATIVE_READ_COMPLETE &&
        read.topology_ready_for_evaluation &&
        read.libdrm_objects_released) {
        memcpy(out_topology, &read.topology, sizeof(*out_topology));
        return 0;
    }
    if (status == MEISO_K230D_DRM_NATIVE_READ_OPERATION_FAILED &&
        read.error > 0) {
        return read.error;
    }
    return EPROTO;
}

static int native_create_rgb565_dumb(
    void *opaque,
    int fd,
    uint32_t width,
    uint32_t height,
    MeisoK230dDrmDumbBuffer *out_buffer
)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    struct drm_mode_create_dumb request;
    MeisoK230dDrmDumbBuffer candidate;
    int result;

    if (out_buffer == NULL || check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    memset(&request, 0, sizeof(request));
    request.width = width;
    request.height = height;
    request.bpp = 16U;
    request.flags = 0U;
    ++context->ledger.create_dumb_api_call_count;
    errno = 0;
    result = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &request);
    if (result != 0) {
        context->ledger.create_dumb_error = positive_error(result);
        return context->ledger.create_dumb_error;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.handle = request.handle;
    candidate.pitch = request.pitch;
    candidate.size = request.size;
    memcpy(out_buffer, &candidate, sizeof(candidate));
    return 0;
}

static int native_add_rgb565_framebuffer(
    void *opaque,
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t handle,
    uint32_t pitch,
    uint32_t *out_framebuffer_id
)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    uint32_t handles[4] = {0U, 0U, 0U, 0U};
    uint32_t pitches[4] = {0U, 0U, 0U, 0U};
    uint32_t offsets[4] = {0U, 0U, 0U, 0U};
    uint32_t framebuffer_id = 0U;
    int result;

    if (out_framebuffer_id == NULL || check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    handles[0] = handle;
    pitches[0] = pitch;
    ++context->ledger.add_framebuffer_call_count;
    errno = 0;
    result = drmModeAddFB2(
        fd, width, height, DRM_FORMAT_RGB565,
        handles, pitches, offsets, &framebuffer_id, 0U
    );
    if (result != 0) {
        context->ledger.add_framebuffer_error = positive_error(result);
        return context->ledger.add_framebuffer_error;
    }
    *out_framebuffer_id = framebuffer_id;
    return 0;
}

static MeisoK230dDrmSinglePlaneTestOnlyCallDisposition native_atomic_test_only(
    void *opaque,
    int fd,
    const MeisoK230dDrmAtomicPlaneState *state,
    int *out_error
)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    drmModeAtomicReqPtr request = NULL;
    uint32_t index;
    int result;
    int error;

    if (context != NULL) {
        ++context->ledger.portable_atomic_callback_call_count;
    }
    if (state == NULL || out_error == NULL ||
        check_same_fd(context, fd) != 0) {
        if (out_error != NULL) {
            *out_error = EPROTO;
        }
        return
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED;
    }
    ++context->ledger.atomic_request_alloc_call_count;
    errno = 0;
    request = drmModeAtomicAlloc();
    if (request == NULL) {
        context->ledger.atomic_request_alloc_error = positive_error(-1);
        *out_error = context->ledger.atomic_request_alloc_error;
        return
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED;
    }
    context->ledger.atomic_request_alloc_succeeded = true;

    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT; ++index) {
        if (state->plane_id == 0U ||
            state->properties[index].property_id == 0U) {
            error = EPROTO;
            context->ledger.atomic_plane_property_add_error = error;
            goto operation_failed;
        }
        ++context->ledger.atomic_plane_property_add_attempt_count;
        errno = 0;
        result = drmModeAtomicAddProperty(
            request,
            state->plane_id,
            state->properties[index].property_id,
            state->properties[index].value
        );
        if (result < 0) {
            error = positive_error(result);
            context->ledger.atomic_plane_property_add_error = error;
            goto operation_failed;
        }
        ++context->ledger.atomic_plane_property_add_success_count;
    }

    ++context->ledger.atomic_commit_api_call_count;
    context->ledger.atomic_commit_flags = DRM_MODE_ATOMIC_TEST_ONLY;
    context->ledger.atomic_commit_user_data_null = true;
    errno = 0;
    result = drmModeAtomicCommit(
        fd, request, DRM_MODE_ATOMIC_TEST_ONLY, NULL
    );
    error = result == 0 ? 0 : positive_error(result);
    context->ledger.atomic_commit_error = error;
    drmModeAtomicFree(request);
    ++context->ledger.atomic_request_free_call_count;
    if (result == 0) {
        *out_error = 0;
        return
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_ACCEPTED_NOT_APPLIED;
    }
    *out_error = error;
    if (error == EINVAL) {
        return
            MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_REJECTED_NOT_APPLIED;
    }
    return
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED;

operation_failed:
    drmModeAtomicFree(request);
    ++context->ledger.atomic_request_free_call_count;
    *out_error = error;
    return
        MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_CALL_OPERATION_FAILED_NOT_APPLIED;
}

static int native_remove_framebuffer(
    void *opaque,
    int fd,
    uint32_t framebuffer_id
)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    int result;
    int error;

    if (check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    ++context->ledger.remove_framebuffer_call_count;
    errno = 0;
    result = drmModeRmFB(fd, framebuffer_id);
    error = result == 0 ? 0 : positive_error(result);
    context->ledger.remove_framebuffer_error = error;
    return error;
}

static int native_destroy_dumb(void *opaque, int fd, uint32_t handle)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    struct drm_mode_destroy_dumb request;
    int result;
    int error;

    if (check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    memset(&request, 0, sizeof(request));
    request.handle = handle;
    ++context->ledger.destroy_dumb_api_call_count;
    errno = 0;
    result = drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &request);
    error = result == 0 ? 0 : positive_error(result);
    context->ledger.destroy_dumb_error = error;
    return error;
}

static int native_drop_master(void *opaque, int fd)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    int result;
    int error;

    if (check_same_fd(context, fd) != 0) {
        return EPROTO;
    }
    ++context->ledger.drop_master_call_count;
    errno = 0;
    result = drmDropMaster(fd);
    if (result != 0) {
        error = positive_error(result);
        context->ledger.drop_master_error = error;
        return error;
    }
    return 0;
}

static int native_close_device(void *opaque, int fd)
{
    NativeTestOnlyContext *context = (NativeTestOnlyContext *)opaque;
    int same_fd_error;
    int close_fd;
    int result;
    int error = 0;

    if (context == NULL) {
        return EPROTO;
    }
    same_fd_error = check_same_fd(context, fd);
    close_fd = context->owned_fd_active ? context->owned_fd : fd;
    ++context->ledger.close_call_count;
    errno = 0;
    result = close(close_fd);
    context->owned_fd_active = false;
    context->ledger.close_terminal_consumed = true;
    if (result != 0) {
        error = positive_error(result);
    }
    context->ledger.close_error = error;
    if (same_fd_error != 0) {
        return same_fd_error;
    }
    return error;
}

static const MeisoK230dDrmSinglePlaneTestOnlyOps NATIVE_OPS = {
    native_open_primary,
    native_enable_universal_planes,
    native_enable_atomic,
    native_acquire_master,
    native_verify_master,
    native_read_topology,
    native_create_rgb565_dumb,
    native_add_rgb565_framebuffer,
    native_atomic_test_only,
    native_remove_framebuffer,
    native_destroy_dumb,
    native_drop_master,
    native_close_device
};

static bool ranges_overlap(
    const void *lhs,
    size_t lhs_size,
    const void *rhs,
    size_t rhs_size
)
{
    return meiso_k230d_drm_private_ranges_overlap(
        lhs, lhs_size, rhs, rhs_size
    );
}

static MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
run_native_single_plane_test_only_validated(
    const char *device_path,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest
        *derive_request,
    bool derive_unique_plane,
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
)
{
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult candidate;
    MeisoK230dDrmSinglePlaneTestOnlyConfig config;
    MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus status;
    NativeTestOnlyContext context;

    memset(&candidate, 0, sizeof(candidate));
    memset(&context, 0, sizeof(context));
    context.owned_fd = -1;
    memset(&config, 0, sizeof(config));
    config.device_path = device_path;
    config.ops = &NATIVE_OPS;
    config.ops_context = &context;
    if (derive_unique_plane) {
        status =
            meiso_k230d_drm_private_run_single_plane_test_only_derive_unique_plane(
                &config, derive_request, &candidate.portable
            );
    } else {
        status = meiso_k230d_drm_private_run_single_plane_test_only(
            &config, request, &candidate.portable
        );
    }
    if (status != MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_RESULT_AVAILABLE) {
        return status;
    }
    memcpy(&candidate.native, &context.ledger, sizeof(candidate.native));
    memcpy(out_result, &candidate, sizeof(candidate));
    return status;
}

MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
meiso_k230d_drm_libdrm_run_single_plane_test_only(
    const char *device_path,
    const MeisoK230dDrmSinglePlaneTestCandidateRequest *request,
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
)
{
    size_t path_size;

    if (device_path == NULL || request == NULL || out_result == NULL ||
        ranges_overlap(request, sizeof(*request), out_result,
            sizeof(*out_result)) ||
        ranges_overlap(device_path, 1U, request, sizeof(*request)) ||
        ranges_overlap(device_path, 1U, out_result, sizeof(*out_result)) ||
        !meiso_k230d_drm_private_path_size(device_path, &path_size) ||
        ranges_overlap(
            device_path, path_size + 1U, request, sizeof(*request)
        ) ||
        ranges_overlap(
            device_path, path_size + 1U, out_result, sizeof(*out_result)
        )) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT;
    }

    return run_native_single_plane_test_only_validated(
        device_path, request, NULL, false, out_result
    );
}

MeisoK230dDrmSinglePlaneTestOnlyInvokeStatus
meiso_k230d_drm_libdrm_run_single_plane_test_only_derive_unique_plane(
    const char *device_path,
    const MeisoK230dDrmSinglePlaneTestDeriveUniquePlaneRequest *request,
    MeisoK230dDrmLibdrmSinglePlaneTestOnlyResult *out_result
)
{
    size_t path_size;

    if (device_path == NULL || request == NULL || out_result == NULL ||
        ranges_overlap(request, sizeof(*request), out_result,
            sizeof(*out_result)) ||
        ranges_overlap(device_path, 1U, request, sizeof(*request)) ||
        ranges_overlap(device_path, 1U, out_result, sizeof(*out_result)) ||
        !meiso_k230d_drm_private_path_size(device_path, &path_size) ||
        ranges_overlap(
            device_path, path_size + 1U, request, sizeof(*request)
        ) ||
        ranges_overlap(
            device_path, path_size + 1U, out_result, sizeof(*out_result)
        )) {
        return MEISO_K230D_DRM_SINGLE_PLANE_TEST_ONLY_INVALID_ARGUMENT;
    }

    return run_native_single_plane_test_only_validated(
        device_path, NULL, request, true, out_result
    );
}
