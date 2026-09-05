#include <meiso/edge/linux/drm_scanout.h>

#include "meiso_drm_scanout_private.h"

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
    FAKE_FD = 41,
    FAKE_DUMB_HANDLE = 70,
    FAKE_FRAMEBUFFER_ID = 80,
    FAKE_LOG_CAPACITY = 96,
    FAKE_EVENT_CAPACITY = 12,
    FAKE_ATOMIC_CAPACITY = 12,
    FIXTURE_ARENA_CAPACITY = 32768,
    FIXTURE_MAPPING_CAPACITY = 2048
};

typedef uint8_t FakeOperation;
enum {
    FAKE_OP_OPEN = 1,
    FAKE_OP_CAP_UNIVERSAL = 2,
    FAKE_OP_CAP_ATOMIC = 3,
    FAKE_OP_MASTER = 4,
    FAKE_OP_TOPOLOGY = 5,
    FAKE_OP_CREATE = 6,
    FAKE_OP_MAP = 7,
    FAKE_OP_ADD_FB = 8,
    FAKE_OP_TEST = 9,
    FAKE_OP_COMMIT = 10,
    FAKE_OP_POLL = 11,
    FAKE_OP_READBACK = 12,
    FAKE_OP_REMOVE_FB = 13,
    FAKE_OP_UNMAP = 14,
    FAKE_OP_DESTROY = 15,
    FAKE_OP_DROP_MASTER = 16,
    FAKE_OP_CLOSE = 17
};

typedef uint8_t FakeCookieKind;
enum {
    FAKE_COOKIE_UNRELATED = 0,
    FAKE_COOKIE_PRESENT = 1,
    FAKE_COOKIE_RESTORE = 2
};

typedef struct {
    MeisoK230dDrmEventPollStatus status;
    FakeCookieKind cookie_kind;
    uint32_t crtc_id;
    uint32_t sequence;
    uint64_t monotonic_ns;
    int error;
} FakeEvent;

typedef struct {
    MeisoK230dDrmTopology topology;
    MeisoK230dDrmPlane readback;
    MeisoK230dDrmDumbBuffer dumb;
    int unrelated_cookie;
    int open_error;
    int universal_capability_error;
    int atomic_capability_error;
    int master_error;
    int topology_error;
    int create_error;
    int map_error;
    int add_fb_error;
    int readback_error;
    int remove_error;
    int unmap_error;
    int destroy_error;
    int drop_error;
    int close_error;
    bool remove_error_once;
    bool unmap_error_once;
    bool destroy_error_once;
    bool drop_error_once;
    void *mapping_result;
    MeisoK230dDrmCommitDisposition dispositions[FAKE_ATOMIC_CAPACITY];
    int disposition_errors[FAKE_ATOMIC_CAPACITY];
    MeisoK230dDrmAtomicPlaneState atomic_states[FAKE_ATOMIC_CAPACITY];
    MeisoK230dDrmAtomicKind atomic_kinds[FAKE_ATOMIC_CAPACITY];
    void *atomic_cookies[FAKE_ATOMIC_CAPACITY];
    uint32_t atomic_count;
    void *present_cookie;
    void *restore_cookie;
    FakeEvent events[FAKE_EVENT_CAPACITY];
    uint32_t event_count;
    uint32_t event_cursor;
    FakeOperation log[FAKE_LOG_CAPACITY];
    uint32_t log_count;
    uint32_t close_calls;
} FakeDrm;

typedef union {
    max_align_t alignment;
    uint8_t bytes[FIXTURE_ARENA_CAPACITY];
} FixtureArena;

typedef struct {
    FakeDrm fake;
    FixtureArena arena;
    uint8_t mapping[FIXTURE_MAPPING_CAPACITY];
    MeisoK230dDrmScanout *scanout;
} Fixture;

typedef struct {
    uint8_t before[16];
    MeisoK230dDrmFrameView frame;
    uint8_t after[48];
} GuardedFrame;

static void log_operation(FakeDrm *fake, FakeOperation operation)
{
    if (fake->log_count < FAKE_LOG_CAPACITY) {
        fake->log[fake->log_count++] = operation;
    }
}

static int fake_open(void *context, const char *path, int *out_fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OP_OPEN);
    if (fake->open_error != 0) {
        return fake->open_error;
    }
    if (strcmp(path, "/dev/dri/card0") != 0) {
        return EINVAL;
    }
    *out_fd = FAKE_FD;
    return 0;
}

static int fake_capability(
    void *context,
    int fd,
    MeisoK230dDrmClientCapability capability
)
{
    FakeDrm *fake = (FakeDrm *)context;
    if (fd != FAKE_FD) {
        return EBADF;
    }
    log_operation(
        fake,
        capability == MEISO_K230D_DRM_CLIENT_UNIVERSAL_PLANES ?
            FAKE_OP_CAP_UNIVERSAL : FAKE_OP_CAP_ATOMIC
    );
    return capability == MEISO_K230D_DRM_CLIENT_UNIVERSAL_PLANES ?
        fake->universal_capability_error : fake->atomic_capability_error;
}

static int fake_master(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OP_MASTER);
    return fd == FAKE_FD ? fake->master_error : EBADF;
}

static int fake_topology(
    void *context,
    int fd,
    MeisoK230dDrmTopology *out_topology
)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OP_TOPOLOGY);
    if (fd != FAKE_FD) {
        return EBADF;
    }
    if (fake->topology_error != 0) {
        return fake->topology_error;
    }
    *out_topology = fake->topology;
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
    log_operation(fake, FAKE_OP_CREATE);
    if (fd != FAKE_FD || width != 4U || height != 4U) {
        return EINVAL;
    }
    if (fake->create_error != 0) {
        return fake->create_error;
    }
    *out_buffer = fake->dumb;
    return 0;
}

static int fake_map(
    void *context,
    int fd,
    uint32_t handle,
    uint64_t size,
    void **out_mapping
)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OP_MAP);
    if (fd != FAKE_FD || handle != fake->dumb.handle ||
        size != fake->dumb.size) {
        return EINVAL;
    }
    if (fake->map_error != 0) {
        return fake->map_error;
    }
    *out_mapping = fake->mapping_result;
    return 0;
}

static int fake_add_fb(
    void *context,
    int fd,
    uint32_t width,
    uint32_t height,
    uint32_t fourcc,
    uint32_t handle,
    uint32_t pitch,
    uint32_t *out_framebuffer_id
)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OP_ADD_FB);
    if (fd != FAKE_FD || width != 4U || height != 4U ||
        fourcc != MEISO_K230D_DRM_RGB565_FOURCC ||
        handle != fake->dumb.handle || pitch != fake->dumb.pitch) {
        return EINVAL;
    }
    if (fake->add_fb_error != 0) {
        return fake->add_fb_error;
    }
    *out_framebuffer_id = FAKE_FRAMEBUFFER_ID;
    return 0;
}

static MeisoK230dDrmCommitDisposition fake_atomic(
    void *context,
    int fd,
    const MeisoK230dDrmAtomicPlaneState *state,
    MeisoK230dDrmAtomicKind kind,
    void *event_cookie,
    int *out_error
)
{
    FakeDrm *fake = (FakeDrm *)context;
    uint32_t index = fake->atomic_count;
    if (fd != FAKE_FD || index >= FAKE_ATOMIC_CAPACITY) {
        *out_error = EINVAL;
        return MEISO_K230D_DRM_COMMIT_DEFINITELY_NOT_APPLIED;
    }
    log_operation(
        fake,
        kind == MEISO_K230D_DRM_ATOMIC_TEST_ONLY ?
            FAKE_OP_TEST : FAKE_OP_COMMIT
    );
    fake->atomic_states[index] = *state;
    fake->atomic_kinds[index] = kind;
    fake->atomic_cookies[index] = event_cookie;
    if (kind == MEISO_K230D_DRM_ATOMIC_PAGE_FLIP_EVENT) {
        if (state->properties[MEISO_K230D_DRM_PLANE_FB_ID].value ==
            FAKE_FRAMEBUFFER_ID) {
            fake->present_cookie = event_cookie;
        } else {
            fake->restore_cookie = event_cookie;
        }
    }
    ++fake->atomic_count;
    *out_error = fake->disposition_errors[index];
    return fake->dispositions[index];
}

static MeisoK230dDrmEventPollStatus fake_poll(
    void *context,
    int fd,
    MeisoK230dDrmPageFlipEvent *out_event,
    int *out_error
)
{
    FakeDrm *fake = (FakeDrm *)context;
    FakeEvent event;
    log_operation(fake, FAKE_OP_POLL);
    if (fd != FAKE_FD || fake->event_cursor >= fake->event_count) {
        return MEISO_K230D_DRM_EVENT_WOULD_BLOCK;
    }
    event = fake->events[fake->event_cursor++];
    if (event.status == MEISO_K230D_DRM_EVENT_TIMEOUT ||
        event.status == MEISO_K230D_DRM_EVENT_FAILED) {
        *out_error = event.error;
    } else if (event.status == MEISO_K230D_DRM_EVENT) {
        out_event->cookie = event.cookie_kind == FAKE_COOKIE_PRESENT ?
            fake->present_cookie :
            (event.cookie_kind == FAKE_COOKIE_RESTORE ?
             fake->restore_cookie : &fake->unrelated_cookie);
        out_event->crtc_id = event.crtc_id;
        out_event->sequence = event.sequence;
        out_event->monotonic_ns = event.monotonic_ns;
    }
    return event.status;
}

static int fake_readback(
    void *context,
    int fd,
    uint32_t plane_id,
    MeisoK230dDrmPlane *out_plane
)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OP_READBACK);
    if (fd != FAKE_FD || plane_id != fake->readback.plane_id) {
        return EINVAL;
    }
    if (fake->readback_error != 0) {
        return fake->readback_error;
    }
    *out_plane = fake->readback;
    return 0;
}

static int fake_remove_fb(void *context, int fd, uint32_t framebuffer_id)
{
    FakeDrm *fake = (FakeDrm *)context;
    int result = fake->remove_error;
    log_operation(fake, FAKE_OP_REMOVE_FB);
    if (fd != FAKE_FD || framebuffer_id != FAKE_FRAMEBUFFER_ID) {
        return EINVAL;
    }
    if (fake->remove_error_once) {
        fake->remove_error = 0;
        fake->remove_error_once = false;
    }
    return result;
}

static int fake_unmap(void *context, void *mapping, uint64_t size)
{
    FakeDrm *fake = (FakeDrm *)context;
    int result = fake->unmap_error;
    log_operation(fake, FAKE_OP_UNMAP);
    if (mapping != fake->mapping_result || size != fake->dumb.size) {
        return EINVAL;
    }
    if (fake->unmap_error_once) {
        fake->unmap_error = 0;
        fake->unmap_error_once = false;
    }
    return result;
}

static int fake_destroy(void *context, int fd, uint32_t handle)
{
    FakeDrm *fake = (FakeDrm *)context;
    int result = fake->destroy_error;
    log_operation(fake, FAKE_OP_DESTROY);
    if (fd != FAKE_FD || handle != fake->dumb.handle) {
        return EINVAL;
    }
    if (fake->destroy_error_once) {
        fake->destroy_error = 0;
        fake->destroy_error_once = false;
    }
    return result;
}

static int fake_drop(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    int result;
    log_operation(fake, FAKE_OP_DROP_MASTER);
    if (fd != FAKE_FD) {
        return EBADF;
    }
    result = fake->drop_error;
    if (fake->drop_error_once) {
        fake->drop_error = 0;
        fake->drop_error_once = false;
    }
    return result;
}

static int fake_close(void *context, int fd)
{
    FakeDrm *fake = (FakeDrm *)context;
    log_operation(fake, FAKE_OP_CLOSE);
    ++fake->close_calls;
    return fd == FAKE_FD ? fake->close_error : EBADF;
}

static const MeisoK230dDrmOps FAKE_OPS = {
    fake_open,
    fake_capability,
    fake_master,
    fake_topology,
    fake_create,
    fake_map,
    fake_add_fb,
    fake_atomic,
    fake_poll,
    fake_readback,
    fake_remove_fb,
    fake_unmap,
    fake_destroy,
    fake_drop,
    fake_close
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

static void fake_init(FakeDrm *fake)
{
    uint32_t index;
    MeisoK230dDrmTopology *topology;
    memset(fake, 0, sizeof(*fake));
    topology = &fake->topology;
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
    fake->readback = topology->planes[1];
    fake->dumb.handle = FAKE_DUMB_HANDLE;
    fake->dumb.pitch = 8U;
    fake->dumb.size = 32U;
    for (index = 0U; index < FAKE_ATOMIC_CAPACITY; ++index) {
        fake->dispositions[index] = MEISO_K230D_DRM_COMMIT_ACCEPTED;
    }
}

static bool fixture_init(Fixture *fixture)
{
    MeisoK230dDrmConfig config;
    fake_init(&fixture->fake);
    memset(fixture->mapping, 0xa5, sizeof(fixture->mapping));
    fixture->fake.mapping_result = fixture->mapping;
    memset(&fixture->arena, 0xa5, sizeof(fixture->arena));
    fixture->scanout = NULL;
    config.device_path = "/dev/dri/card0";
    config.ops = &FAKE_OPS;
    config.ops_context = &fixture->fake;
    CHECK(meiso_k230d_drm_scanout_memory_required() <=
          sizeof(fixture->arena.bytes));
    CHECK(meiso_k230d_drm_scanout_init(
              fixture->arena.bytes,
              sizeof(fixture->arena.bytes),
              &config,
              &fixture->scanout
          ) == MEISO_K230D_DRM_INIT_OK);
    CHECK(fixture->scanout != NULL);
    return true;
}

static MeisoK230dDrmFrameView rgba_frame(void)
{
    static const uint8_t pixels[8] = {
        255U, 0U, 0U, 255U,
        0U, 255U, 0U, 255U
    };
    MeisoK230dDrmFrameView frame = {
        MEISO_K230D_DRM_INPUT_RGBA8888, 2U, 1U, pixels, sizeof(pixels)
    };
    return frame;
}

static void push_event(
    FakeDrm *fake,
    MeisoK230dDrmEventPollStatus status,
    FakeCookieKind cookie_kind,
    uint32_t crtc_id,
    uint32_t sequence,
    uint64_t monotonic_ns,
    int error
)
{
    FakeEvent *event = &fake->events[fake->event_count++];
    event->status = status;
    event->cookie_kind = cookie_kind;
    event->crtc_id = crtc_id;
    event->sequence = sequence;
    event->monotonic_ns = monotonic_ns;
    event->error = error;
}

static bool bytes_are(const uint8_t *bytes, uint8_t value, size_t size)
{
    size_t index;
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

static uint32_t operation_count(
    const FakeDrm *fake,
    FakeOperation operation
)
{
    uint32_t count = 0U;
    uint32_t index;
    for (index = 0U; index < fake->log_count; ++index) {
        if (fake->log[index] == operation) {
            ++count;
        }
    }
    return count;
}

static bool operations_equal(
    const FakeDrm *fake,
    const FakeOperation *expected,
    size_t expected_count
)
{
    if (fake->log_count != expected_count) {
        return false;
    }
    return expected_count == 0U ||
        memcmp(fake->log, expected, expected_count * sizeof(expected[0])) == 0;
}

static bool expect_topology_blocked(Fixture *fixture)
{
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;
    CHECK(meiso_k230d_drm_scanout_begin(fixture->scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture->scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL);
    CHECK(fixture->fake.atomic_count == 0U);
    CHECK(operation_count(&fixture->fake, FAKE_OP_COMMIT) == 0U);
    return true;
}

static bool test_successful_rgba_present_and_restore(void)
{
    static const uint8_t expected_input_sha[32] = {
        0x8e, 0x56, 0x46, 0x7a, 0x23, 0xff, 0x16, 0xf4,
        0x05, 0x9b, 0x73, 0x84, 0x17, 0x08, 0x1a, 0xbf,
        0x48, 0x60, 0x0e, 0x4d, 0x0d, 0x99, 0x58, 0x21,
        0x71, 0x78, 0xf2, 0xd5, 0xd4, 0xca, 0x93, 0xf8
    };
    static const uint8_t expected_topology_sha[32] = {
        0xbe, 0x58, 0xf2, 0x96, 0x5f, 0xd9, 0x72, 0x72,
        0x61, 0x9b, 0x82, 0x4b, 0x15, 0xb3, 0x03, 0xb0,
        0x1f, 0x0e, 0xa9, 0xc1, 0xed, 0x35, 0x8b, 0xa6,
        0x3b, 0x50, 0xf5, 0xe8, 0x95, 0xed, 0x93, 0x11
    };
    Fixture fixture;
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;
    uint32_t row;
    uint32_t column;

    CHECK(fixture_init(&fixture));
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    CHECK(fixture.fake.log_count >= 4U);
    CHECK(fixture.fake.log[0] == FAKE_OP_OPEN &&
          fixture.fake.log[1] == FAKE_OP_CAP_UNIVERSAL &&
          fixture.fake.log[2] == FAKE_OP_CAP_ATOMIC &&
          fixture.fake.log[3] == FAKE_OP_MASTER);
    CHECK(fixture.fake.atomic_count == 2U);
    CHECK(fixture.fake.atomic_kinds[0] ==
          MEISO_K230D_DRM_ATOMIC_TEST_ONLY);
    CHECK(fixture.fake.atomic_cookies[0] == NULL);
    CHECK(fixture.fake.atomic_kinds[1] ==
          MEISO_K230D_DRM_ATOMIC_PAGE_FLIP_EVENT);
    CHECK(fixture.fake.atomic_cookies[1] != NULL);
    CHECK(fixture.fake.atomic_states[1].plane_id == 50U);
    CHECK(fixture.fake.atomic_states[1].properties[
              MEISO_K230D_DRM_PLANE_FB_ID
          ].value == FAKE_FRAMEBUFFER_ID);
    CHECK(fixture.fake.atomic_states[1].properties[
              MEISO_K230D_DRM_PLANE_CRTC_ID
          ].value == 30U);
    CHECK(fixture.fake.atomic_states[1].properties[
              MEISO_K230D_DRM_PLANE_SRC_W
          ].value == (UINT64_C(4) << 16U));
    CHECK(bytes_are(fixture.mapping, 0U, 8U));
    CHECK(bytes_are(fixture.mapping + 24U, 0U, 8U));
    for (row = 1U; row <= 2U; ++row) {
        for (column = 0U; column < 2U; ++column) {
            CHECK(fixture.mapping[row * 8U + column * 2U] == 0x00U);
            CHECK(fixture.mapping[row * 8U + column * 2U + 1U] == 0xf8U);
        }
        for (column = 2U; column < 4U; ++column) {
            CHECK(fixture.mapping[row * 8U + column * 2U] == 0xe0U);
            CHECK(fixture.mapping[row * 8U + column * 2U + 1U] == 0x07U);
        }
    }
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_PRESENT_PENDING);
    CHECK(snapshot.scanout.receipt_valid);
    CHECK(!snapshot.scanout.physical_evidence);
    CHECK(snapshot.scanout.evidence_class ==
          MEISO_K230D_DRM_EVIDENCE_INJECTED_OPS_UNVERIFIED);
    CHECK(memcmp(snapshot.scanout.input_sha256, expected_input_sha,
                 sizeof(expected_input_sha)) == 0);
    CHECK(memcmp(snapshot.scanout.topology_sha256, expected_topology_sha,
                 sizeof(expected_topology_sha)) == 0);
    CHECK(snapshot.scanout.viewport_x == 0U &&
          snapshot.scanout.viewport_y == 1U &&
          snapshot.scanout.viewport_width == 4U &&
          snapshot.scanout.viewport_height == 2U);

    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT_WOULD_BLOCK,
        FAKE_COOKIE_UNRELATED, 0U, 0U, 0U, 0
    );
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_UNRELATED, 30U, 8U, 800U, 0
    );
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_PRESENT, 30U, 9U, 900U, 0
    );
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture.scanout) ==
          MEISO_K230D_DRM_POLL_WOULD_BLOCK);
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture.scanout) ==
          MEISO_K230D_DRM_POLL_WOULD_BLOCK);
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture.scanout) ==
          MEISO_K230D_DRM_POLL_KMS_LATCHED);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    CHECK(fixture.fake.atomic_count == 4U);
    {
        uint32_t property_index;
        for (property_index = 0U;
             property_index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
             ++property_index) {
            CHECK(fixture.fake.atomic_states[2].properties[property_index].
                      property_id == fixture.fake.topology.planes[1].
                          properties[property_index].property_id);
            CHECK(fixture.fake.atomic_states[2].properties[property_index].
                      value == fixture.fake.topology.planes[1].
                          properties[property_index].value);
            CHECK(fixture.fake.atomic_states[3].properties[property_index].
                      property_id == fixture.fake.topology.planes[1].
                          properties[property_index].property_id);
            CHECK(fixture.fake.atomic_states[3].properties[property_index].
                      value == fixture.fake.topology.planes[1].
                          properties[property_index].value);
        }
    }
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_PRESENT, 30U, 10U, 1000U, 0
    );
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 11U, 1100U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_WOULD_BLOCK);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_COMPLETE);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_STOPPED);
    CHECK(!snapshot.fd_owned && !snapshot.master_owned &&
          !snapshot.dumb_owned && !snapshot.mapping_owned &&
          !snapshot.framebuffer_owned);
    CHECK(snapshot.scanout.kms_latched &&
          snapshot.scanout.matching_event_observed &&
          snapshot.scanout.event_sequence == 9U);
    CHECK(snapshot.cleanup.restore_test_accepted &&
          snapshot.cleanup.restore_commit_accepted &&
          snapshot.cleanup.matching_event_observed &&
          snapshot.cleanup.property_readback_matched &&
          snapshot.cleanup.framebuffer_removed &&
          snapshot.cleanup.mapping_unmapped &&
          snapshot.cleanup.dumb_buffer_destroyed &&
          snapshot.cleanup.master_dropped &&
          snapshot.cleanup.device_closed &&
          !snapshot.cleanup.retained_resources);
    CHECK(fixture.fake.log[fixture.fake.log_count - 5U] ==
          FAKE_OP_REMOVE_FB);
    CHECK(fixture.fake.log[fixture.fake.log_count - 4U] == FAKE_OP_UNMAP);
    CHECK(fixture.fake.log[fixture.fake.log_count - 3U] == FAKE_OP_DESTROY);
    CHECK(fixture.fake.log[fixture.fake.log_count - 2U] ==
          FAKE_OP_DROP_MASTER);
    CHECK(fixture.fake.log[fixture.fake.log_count - 1U] == FAKE_OP_CLOSE);
    return true;
}

static bool test_restore_preserves_nonzero_detached_geometry(void)
{
    static const uint64_t original_geometry[8] = {
        UINT64_C(0x00010000), UINT64_C(0x00020000),
        UINT64_C(0x00030000), UINT64_C(0x00040000),
        UINT64_C(5), UINT64_C(6), UINT64_C(7), UINT64_C(8)
    };
    Fixture fixture;
    MeisoK230dDrmFrameView frame = rgba_frame();
    uint32_t index;

    CHECK(fixture_init(&fixture));
    for (index = MEISO_K230D_DRM_PLANE_SRC_X;
         index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        fixture.fake.topology.planes[1].properties[index].value =
            original_geometry[index - MEISO_K230D_DRM_PLANE_SRC_X];
    }
    fixture.fake.readback = fixture.fake.topology.planes[1];
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_PRESENT, 30U, 1U, 100U, 0
    );
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture.scanout) ==
          MEISO_K230D_DRM_POLL_KMS_LATCHED);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    CHECK(fixture.fake.atomic_count == 4U);
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        CHECK(fixture.fake.atomic_states[2].properties[index].property_id ==
              fixture.fake.topology.planes[1].properties[index].property_id);
        CHECK(fixture.fake.atomic_states[2].properties[index].value ==
              fixture.fake.topology.planes[1].properties[index].value);
        CHECK(fixture.fake.atomic_states[3].properties[index].property_id ==
              fixture.fake.topology.planes[1].properties[index].property_id);
        CHECK(fixture.fake.atomic_states[3].properties[index].value ==
              fixture.fake.topology.planes[1].properties[index].value);
    }
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_COMPLETE);
    return true;
}

static bool test_rgb565_be_conversion(void)
{
    static const uint8_t pixels[4] = {0xf8U, 0x00U, 0x00U, 0x1fU};
    Fixture fixture;
    MeisoK230dDrmFrameView frame = {
        MEISO_K230D_DRM_INPUT_RGB565_BE, 1U, 2U, pixels, sizeof(pixels)
    };
    uint32_t row;
    CHECK(fixture_init(&fixture));
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    for (row = 0U; row < 4U; ++row) {
        const uint8_t low = row < 2U ? 0x00U : 0x1fU;
        const uint8_t high = row < 2U ? 0xf8U : 0x00U;
        CHECK(fixture.mapping[row * 8U] == 0U &&
              fixture.mapping[row * 8U + 1U] == 0U);
        CHECK(fixture.mapping[row * 8U + 2U] == low &&
              fixture.mapping[row * 8U + 3U] == high);
        CHECK(fixture.mapping[row * 8U + 4U] == low &&
              fixture.mapping[row * 8U + 5U] == high);
        CHECK(fixture.mapping[row * 8U + 6U] == 0U &&
              fixture.mapping[row * 8U + 7U] == 0U);
    }
    CHECK(meiso_k230d_drm_scanout_declare_present_outcome_unknown(
              fixture.scanout, ETIMEDOUT
          ));
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    CHECK(meiso_k230d_drm_scanout_declare_restore_outcome_unknown(
              fixture.scanout, ETIMEDOUT
          ));
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN);
    return true;
}

static bool test_input_and_topology_admission(void)
{
    Fixture fixture;
    MeisoK230dDrmFrameView frame;
    uint8_t nonopaque[4] = {1U, 2U, 3U, 4U};
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    frame.bytes = nonopaque;
    frame.width = 1U;
    frame.size = sizeof(nonopaque);
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(fixture.fake.log_count == 0U);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_INPUT_NOT_OPAQUE);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.topology.page_flip_timestamps_monotonic = false;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_PAGE_FLIP_CLOCK_UNPROVABLE);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.topology.planes[1].possible_crtcs = 2U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.topology.planes[1].zpos = 0;
    fixture.fake.readback = fixture.fake.topology.planes[1];
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.topology.planes[1].type =
        MEISO_K230D_DRM_PLANE_TYPE_PRIMARY;
    fixture.fake.readback = fixture.fake.topology.planes[1];
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.topology.crtcs[0].active = false;
    fixture.fake.topology.crtcs[0].mode_valid = false;
    memset(&fixture.fake.topology.crtcs[0].mode, 0,
           sizeof(fixture.fake.topology.crtcs[0].mode));
    fixture.fake.topology.crtcs[0].framebuffer_id = 0U;
    fixture.fake.topology.planes[0].properties[
        MEISO_K230D_DRM_PLANE_FB_ID
    ].value = 0U;
    fixture.fake.topology.planes[0].properties[
        MEISO_K230D_DRM_PLANE_CRTC_ID
    ].value = 0U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_NO_ACTIVE_ROUTE);
    return true;
}

static bool test_topology_canonical_contract(void)
{
    Fixture fixture;
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.connector_count = 2U;
    fixture.fake.topology.connectors[1] =
        fixture.fake.topology.connectors[0];
    fixture.fake.topology.connectors[1].connector_id = 11U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_MULTIPLE_ACTIVE_ROUTES);
    CHECK(fixture.fake.atomic_count == 0U);

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.connector_count = 2U;
    fixture.fake.topology.connectors[1] =
        fixture.fake.topology.connectors[0];
    fixture.fake.topology.connectors[1].connector_id = 9U;
    fixture.fake.topology.connectors[1].connected = false;
    CHECK(expect_topology_blocked(&fixture));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.crtc_count = 2U;
    fixture.fake.topology.crtcs[1].crtc_id = 20U;
    CHECK(expect_topology_blocked(&fixture));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[0].plane_id = 60U;
    CHECK(expect_topology_blocked(&fixture));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.crtcs[0].active = false;
    fixture.fake.topology.crtcs[0].mode_valid = false;
    fixture.fake.topology.crtcs[0].framebuffer_id = 0U;
    memset(
        &fixture.fake.topology.crtcs[0].mode,
        0,
        sizeof(fixture.fake.topology.crtcs[0].mode)
    );
    CHECK(expect_topology_blocked(&fixture));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[0].properties[
        MEISO_K230D_DRM_PLANE_CRTC_ID
    ].value = 31U;
    CHECK(expect_topology_blocked(&fixture));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[0].possible_crtcs = 0U;
    CHECK(expect_topology_blocked(&fixture));

    /*
     * Bits refer to the canonical sorted CRTC array. Here the active CRTC is
     * index 1 even though a native libdrm resource list may have ordered it
     * differently before translation.
     */
    CHECK(fixture_init(&fixture));
    fixture.fake.topology.crtc_count = 2U;
    fixture.fake.topology.crtcs[1] = fixture.fake.topology.crtcs[0];
    memset(
        &fixture.fake.topology.crtcs[0],
        0,
        sizeof(fixture.fake.topology.crtcs[0])
    );
    fixture.fake.topology.crtcs[0].crtc_id = 20U;
    fixture.fake.topology.connector_count = 2U;
    fixture.fake.topology.connectors[1] =
        fixture.fake.topology.connectors[0];
    fixture.fake.topology.connectors[1].connector_id = 11U;
    fixture.fake.topology.connectors[1].encoder_id = 21U;
    fixture.fake.topology.connectors[1].crtc_id = 20U;
    fixture.fake.topology.planes[0].possible_crtcs = 2U;
    fixture.fake.topology.planes[1].possible_crtcs = 2U;
    fixture.fake.readback = fixture.fake.topology.planes[1];
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.crtc_id == 30U &&
          snapshot.scanout.crtc_index == 1U);
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_PRESENT, 30U, 1U, 100U, 0
    );
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture.scanout) ==
          MEISO_K230D_DRM_POLL_KMS_LATCHED);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_COMPLETE);
    return true;
}

static bool expect_plane_admission_blocked(
    Fixture *fixture,
    MeisoK230dDrmAdmissionReason reason
)
{
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;
    CHECK(meiso_k230d_drm_scanout_begin(fixture->scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture->scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason == reason);
    CHECK(fixture->fake.atomic_count == 0U);
    CHECK(operation_count(&fixture->fake, FAKE_OP_TEST) == 0U);
    CHECK(operation_count(&fixture->fake, FAKE_OP_COMMIT) == 0U);
    return true;
}

static bool test_plane_safety_admission_matrix(void)
{
    Fixture fixture;

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[1].type_immutable = false;
    CHECK(expect_plane_admission_blocked(
        &fixture, MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[1].zpos_present = false;
    fixture.fake.topology.planes[1].zpos_immutable = false;
    fixture.fake.topology.planes[1].zpos = 0;
    CHECK(expect_plane_admission_blocked(
        &fixture, MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[1].zpos_immutable = false;
    CHECK(expect_plane_admission_blocked(
        &fixture, MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[0].type_immutable = false;
    CHECK(expect_plane_admission_blocked(
        &fixture, MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[0].zpos_present = false;
    fixture.fake.topology.planes[0].zpos_immutable = false;
    fixture.fake.topology.planes[0].zpos = 0;
    CHECK(expect_plane_admission_blocked(
        &fixture, MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[0].zpos_immutable = false;
    CHECK(expect_plane_admission_blocked(
        &fixture, MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[1].properties[
        MEISO_K230D_DRM_PLANE_FB_ID
    ].value = 600U;
    fixture.fake.topology.planes[1].properties[
        MEISO_K230D_DRM_PLANE_CRTC_ID
    ].value = 30U;
    CHECK(expect_plane_admission_blocked(
        &fixture,
        MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology.planes[1].supports_rgb565 = false;
    CHECK(expect_plane_admission_blocked(
        &fixture,
        MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY
    ));
    return true;
}

static bool test_input_mode_and_dumb_bounds(void)
{
    static const uint8_t tiny[2] = {0U, 0U};
    Fixture fixture;
    MeisoK230dDrmFrameView frame = {
        MEISO_K230D_DRM_INPUT_RGB565_BE, 1U, 1U, tiny, sizeof(tiny)
    };
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    frame.width = MEISO_K230D_DRM_MAX_DIMENSION + 1U;
    frame.size = (size_t)frame.width * 2U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(fixture.fake.log_count == 0U);

    CHECK(fixture_init(&fixture));
    frame.width = 1U;
    frame.size = (size_t)MEISO_K230D_DRM_MAX_INPUT_BYTES + 1U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(fixture.fake.log_count == 0U);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.topology.crtcs[0].mode.hdisplay = 4097U;
    fixture.fake.topology.crtcs[0].mode.hsync_start = 4097U;
    fixture.fake.topology.crtcs[0].mode.hsync_end = 4097U;
    fixture.fake.topology.crtcs[0].mode.htotal = 4097U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_MODE_ENVELOPE);
    CHECK(operation_count(&fixture.fake, FAKE_OP_CREATE) == 0U);

    CHECK(fixture_init(&fixture));
    fixture.fake.dumb.pitch = 7U;
    fixture.fake.dumb.size = 28U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(operation_count(&fixture.fake, FAKE_OP_MAP) == 0U);
    CHECK(fixture.fake.atomic_count == 0U);

    CHECK(fixture_init(&fixture));
    fixture.fake.dumb.size = 31U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(operation_count(&fixture.fake, FAKE_OP_MAP) == 0U);

    CHECK(fixture_init(&fixture));
    fixture.fake.dumb.size = MEISO_K230D_DRM_MAX_DUMB_BYTES + 1U;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(operation_count(&fixture.fake, FAKE_OP_MAP) == 0U);
    return true;
}

static bool expect_failed_and_released(
    Fixture *fixture,
    MeisoK230dDrmBeginStatus expected_status,
    MeisoK230dDrmFailureStage expected_stage,
    int expected_error,
    const FakeOperation *expected_operations,
    size_t expected_operation_count
)
{
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;
    CHECK(meiso_k230d_drm_scanout_begin(fixture->scanout, &frame) ==
          expected_status);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture->scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_STOPPED);
    CHECK(snapshot.scanout.failure_stage == expected_stage);
    CHECK(snapshot.scanout.error == expected_error);
    CHECK(!snapshot.fd_owned && !snapshot.master_owned &&
          !snapshot.dumb_owned && !snapshot.mapping_owned &&
          !snapshot.framebuffer_owned &&
          !snapshot.cleanup_callbacks_forbidden &&
          !snapshot.cleanup.retained_resources);
    CHECK(fixture->fake.atomic_count == 0U);
    CHECK(operations_equal(
        &fixture->fake, expected_operations, expected_operation_count
    ));
    return true;
}

static bool test_acquisition_failure_release_matrix(void)
{
    static const FakeOperation open_ops[] = {
        FAKE_OP_OPEN
    };
    static const FakeOperation universal_ops[] = {
        FAKE_OP_OPEN, FAKE_OP_CAP_UNIVERSAL, FAKE_OP_CLOSE
    };
    static const FakeOperation atomic_ops[] = {
        FAKE_OP_OPEN, FAKE_OP_CAP_UNIVERSAL, FAKE_OP_CAP_ATOMIC,
        FAKE_OP_CLOSE
    };
    static const FakeOperation master_ops[] = {
        FAKE_OP_OPEN, FAKE_OP_CAP_UNIVERSAL, FAKE_OP_CAP_ATOMIC,
        FAKE_OP_MASTER, FAKE_OP_CLOSE
    };
    static const FakeOperation topology_ops[] = {
        FAKE_OP_OPEN, FAKE_OP_CAP_UNIVERSAL, FAKE_OP_CAP_ATOMIC,
        FAKE_OP_MASTER, FAKE_OP_TOPOLOGY, FAKE_OP_DROP_MASTER,
        FAKE_OP_CLOSE
    };
    static const FakeOperation create_ops[] = {
        FAKE_OP_OPEN, FAKE_OP_CAP_UNIVERSAL, FAKE_OP_CAP_ATOMIC,
        FAKE_OP_MASTER, FAKE_OP_TOPOLOGY, FAKE_OP_CREATE,
        FAKE_OP_DROP_MASTER, FAKE_OP_CLOSE
    };
    static const FakeOperation map_ops[] = {
        FAKE_OP_OPEN, FAKE_OP_CAP_UNIVERSAL, FAKE_OP_CAP_ATOMIC,
        FAKE_OP_MASTER, FAKE_OP_TOPOLOGY, FAKE_OP_CREATE, FAKE_OP_MAP,
        FAKE_OP_DESTROY, FAKE_OP_DROP_MASTER, FAKE_OP_CLOSE
    };
    static const FakeOperation add_fb_ops[] = {
        FAKE_OP_OPEN, FAKE_OP_CAP_UNIVERSAL, FAKE_OP_CAP_ATOMIC,
        FAKE_OP_MASTER, FAKE_OP_TOPOLOGY, FAKE_OP_CREATE, FAKE_OP_MAP,
        FAKE_OP_ADD_FB, FAKE_OP_UNMAP, FAKE_OP_DESTROY,
        FAKE_OP_DROP_MASTER, FAKE_OP_CLOSE
    };
    Fixture fixture;

    CHECK(fixture_init(&fixture));
    fixture.fake.open_error = EACCES;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED,
        MEISO_K230D_DRM_FAILURE_OPEN, EACCES,
        open_ops, sizeof(open_ops) / sizeof(open_ops[0])
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.universal_capability_error = EOPNOTSUPP;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED,
        MEISO_K230D_DRM_FAILURE_CLIENT_CAPABILITY, EOPNOTSUPP,
        universal_ops, sizeof(universal_ops) / sizeof(universal_ops[0])
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.atomic_capability_error = EOPNOTSUPP;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED,
        MEISO_K230D_DRM_FAILURE_CLIENT_CAPABILITY, EOPNOTSUPP,
        atomic_ops, sizeof(atomic_ops) / sizeof(atomic_ops[0])
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.master_error = EBUSY;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED,
        MEISO_K230D_DRM_FAILURE_ACQUIRE_MASTER, EBUSY,
        master_ops, sizeof(master_ops) / sizeof(master_ops[0])
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.topology_error = EIO;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED,
        MEISO_K230D_DRM_FAILURE_TOPOLOGY, EIO,
        topology_ops, sizeof(topology_ops) / sizeof(topology_ops[0])
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.create_error = ENOMEM;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED,
        MEISO_K230D_DRM_FAILURE_CREATE_DUMB, ENOMEM,
        create_ops, sizeof(create_ops) / sizeof(create_ops[0])
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.map_error = ENOMEM;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED,
        MEISO_K230D_DRM_FAILURE_MAP_DUMB, ENOMEM,
        map_ops, sizeof(map_ops) / sizeof(map_ops[0])
    ));

    CHECK(fixture_init(&fixture));
    fixture.fake.add_fb_error = EINVAL;
    CHECK(expect_failed_and_released(
        &fixture, MEISO_K230D_DRM_BEGIN_OPERATION_FAILED,
        MEISO_K230D_DRM_FAILURE_ADD_FRAMEBUFFER, EINVAL,
        add_fb_ops, sizeof(add_fb_ops) / sizeof(add_fb_ops[0])
    ));
    return true;
}

static bool test_commit_dispositions_and_event_unknown(void)
{
    Fixture fixture;
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    fixture.fake.dispositions[0] =
        MEISO_K230D_DRM_COMMIT_DEFINITELY_NOT_APPLIED;
    fixture.fake.disposition_errors[0] = EINVAL;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_ADMISSION_BLOCKED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.admission_reason ==
          MEISO_K230D_DRM_ADMISSION_ATOMIC_TEST_REJECTED);
    CHECK(fixture.fake.atomic_count == 1U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_COMMIT) == 0U);

    CHECK(fixture_init(&fixture));
    fixture.fake.dispositions[1] =
        MEISO_K230D_DRM_COMMIT_DEFINITELY_NOT_APPLIED;
    fixture.fake.disposition_errors[1] = EINVAL;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_OPERATION_FAILED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_STOPPED);
    CHECK(!snapshot.fd_owned && !snapshot.framebuffer_owned);

    CHECK(fixture_init(&fixture));
    fixture.fake.dispositions[1] =
        MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN;
    fixture.fake.disposition_errors[1] = EIO;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_OUTCOME_UNKNOWN);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase ==
          MEISO_K230D_DRM_PHASE_PRESENT_OUTCOME_UNKNOWN);
    CHECK(snapshot.scanout.outcome_unknown && snapshot.fd_owned &&
          snapshot.framebuffer_owned &&
          snapshot.present_commit_may_be_active);
    CHECK(fixture.fake.close_calls == 0U);

    CHECK(fixture_init(&fixture));
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_PRESENT, 31U, 1U, 100U, 0
    );
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture.scanout) ==
          MEISO_K230D_DRM_POLL_OUTCOME_UNKNOWN);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.outcome_unknown && snapshot.fd_owned &&
          fixture.fake.close_calls == 0U);

    CHECK(fixture_init(&fixture));
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_PRESENT, 30U, 1U, 0U, 0
    );
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture.scanout) ==
          MEISO_K230D_DRM_POLL_OUTCOME_UNKNOWN);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.scanout.failure_stage ==
          MEISO_K230D_DRM_FAILURE_PRESENT_EVENT);
    CHECK(snapshot.scanout.error == EPROTO && snapshot.fd_owned);
    return true;
}

static bool drive_to_latched(Fixture *fixture)
{
    MeisoK230dDrmFrameView frame = rgba_frame();
    CHECK(meiso_k230d_drm_scanout_begin(fixture->scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    push_event(
        &fixture->fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_PRESENT, 30U, 1U, 100U, 0
    );
    CHECK(meiso_k230d_drm_scanout_poll_present(fixture->scanout) ==
          MEISO_K230D_DRM_POLL_KMS_LATCHED);
    return true;
}

static bool drive_to_restore_pending(Fixture *fixture)
{
    CHECK(drive_to_latched(fixture));
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture->scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    return true;
}

static bool finish_restore(Fixture *fixture)
{
    push_event(
        &fixture->fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture->scanout) ==
          MEISO_K230D_DRM_CLEANUP_COMPLETE);
    return true;
}

static bool test_restore_unknown_readback_and_release_retry(void)
{
    Fixture fixture;
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    fixture.fake.dispositions[3] =
        MEISO_K230D_DRM_COMMIT_OUTCOME_UNKNOWN;
    fixture.fake.disposition_errors[3] = EIO;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    CHECK(meiso_k230d_drm_scanout_declare_present_outcome_unknown(
              fixture.scanout, ETIMEDOUT
          ));
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase ==
          MEISO_K230D_DRM_PHASE_RESTORE_OUTCOME_UNKNOWN);
    CHECK(snapshot.cleanup.outcome_unknown && snapshot.fd_owned &&
          snapshot.present_commit_may_be_active &&
          snapshot.restore_commit_may_be_active);
    CHECK(fixture.fake.close_calls == 0U);

    CHECK(fixture_init(&fixture));
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    CHECK(meiso_k230d_drm_scanout_declare_present_outcome_unknown(
              fixture.scanout, ETIMEDOUT
          ));
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_COMPLETE);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_STOPPED);
    CHECK(snapshot.scanout.outcome_unknown &&
          snapshot.cleanup.property_readback_matched &&
          !snapshot.fd_owned && !snapshot.master_owned &&
          !snapshot.dumb_owned && !snapshot.mapping_owned &&
          !snapshot.framebuffer_owned);

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_restore_pending(&fixture));
    fixture.fake.readback.properties[MEISO_K230D_DRM_PLANE_FB_ID].value = 1U;
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.cleanup.matching_event_observed &&
          !snapshot.cleanup.property_readback_matched &&
          snapshot.cleanup.outcome_unknown && snapshot.framebuffer_owned);
    CHECK(fixture.fake.close_calls == 0U);

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_restore_pending(&fixture));
    fixture.fake.remove_error = EBUSY;
    fixture.fake.remove_error_once = true;
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED);
    CHECK(snapshot.framebuffer_owned &&
          !snapshot.present_commit_may_be_active &&
          !snapshot.restore_commit_may_be_active);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_COMPLETE);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_STOPPED &&
          !snapshot.cleanup.retained_resources);
    return true;
}

static bool test_restore_rejected_and_definite_retry(void)
{
    Fixture fixture;
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_latched(&fixture));
    fixture.fake.dispositions[2] =
        MEISO_K230D_DRM_COMMIT_DEFINITELY_NOT_APPLIED;
    fixture.fake.disposition_errors[2] = EINVAL;
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_OPERATION_FAILED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_KMS_LATCHED);
    CHECK(snapshot.cleanup.failure_stage ==
          MEISO_K230D_DRM_FAILURE_RESTORE_TEST);
    CHECK(snapshot.present_commit_may_be_active && snapshot.fd_owned);
    CHECK(fixture.fake.atomic_count == 3U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_REMOVE_FB) == 0U);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    CHECK(fixture.fake.atomic_count == 5U);
    CHECK(finish_restore(&fixture));

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_latched(&fixture));
    fixture.fake.dispositions[3] =
        MEISO_K230D_DRM_COMMIT_DEFINITELY_NOT_APPLIED;
    fixture.fake.disposition_errors[3] = EINVAL;
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_OPERATION_FAILED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_KMS_LATCHED);
    CHECK(snapshot.cleanup.failure_stage ==
          MEISO_K230D_DRM_FAILURE_RESTORE_COMMIT);
    CHECK(snapshot.present_commit_may_be_active &&
          !snapshot.restore_commit_may_be_active && snapshot.fd_owned);
    CHECK(operation_count(&fixture.fake, FAKE_OP_REMOVE_FB) == 0U);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RESTORE_PENDING);
    CHECK(fixture.fake.atomic_count == 6U);
    CHECK(finish_restore(&fixture));

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_restore_pending(&fixture));
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 31U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase ==
          MEISO_K230D_DRM_PHASE_RESTORE_OUTCOME_UNKNOWN);
    CHECK(snapshot.cleanup.failure_stage ==
          MEISO_K230D_DRM_FAILURE_RESTORE_EVENT);
    CHECK(snapshot.framebuffer_owned && fixture.fake.close_calls == 0U);
    return true;
}

static bool test_plane_readback_equality_matrix(void)
{
    FakeDrm fake;
    MeisoK230dDrmPlane actual;
    uint32_t index;

    fake_init(&fake);
    for (index = 0U; index < MEISO_K230D_DRM_PLANE_PROPERTY_COUNT;
         ++index) {
        actual = fake.topology.planes[1];
        ++actual.properties[index].property_id;
        CHECK(!meiso_k230d_drm_private_plane_readback_equal(
            &fake.topology.planes[1], &actual
        ));
        actual = fake.topology.planes[1];
        ++actual.properties[index].value;
        CHECK(!meiso_k230d_drm_private_plane_readback_equal(
            &fake.topology.planes[1], &actual
        ));
    }
    actual = fake.topology.planes[1];
    ++actual.plane_id;
    CHECK(!meiso_k230d_drm_private_plane_readback_equal(
        &fake.topology.planes[1], &actual
    ));
    actual = fake.topology.planes[1];
    actual.type = MEISO_K230D_DRM_PLANE_TYPE_PRIMARY;
    CHECK(!meiso_k230d_drm_private_plane_readback_equal(
        &fake.topology.planes[1], &actual
    ));
    actual = fake.topology.planes[1];
    actual.type_immutable = false;
    CHECK(!meiso_k230d_drm_private_plane_readback_equal(
        &fake.topology.planes[1], &actual
    ));
    actual = fake.topology.planes[1];
    actual.zpos_present = false;
    CHECK(!meiso_k230d_drm_private_plane_readback_equal(
        &fake.topology.planes[1], &actual
    ));
    actual = fake.topology.planes[1];
    actual.zpos_immutable = false;
    CHECK(!meiso_k230d_drm_private_plane_readback_equal(
        &fake.topology.planes[1], &actual
    ));
    actual = fake.topology.planes[1];
    ++actual.zpos;
    CHECK(!meiso_k230d_drm_private_plane_readback_equal(
        &fake.topology.planes[1], &actual
    ));
    return true;
}

static bool test_readback_error_retains_every_resource(void)
{
    Fixture fixture;
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_restore_pending(&fixture));
    fixture.fake.readback_error = EIO;
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_OUTCOME_UNKNOWN);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.cleanup.failure_stage ==
          MEISO_K230D_DRM_FAILURE_RESTORE_READBACK);
    CHECK(snapshot.cleanup.error == EIO && snapshot.fd_owned &&
          snapshot.master_owned && snapshot.dumb_owned &&
          snapshot.mapping_owned && snapshot.framebuffer_owned);
    CHECK(operation_count(&fixture.fake, FAKE_OP_REMOVE_FB) == 0U);
    return true;
}

static bool run_release_failure_case(FakeOperation operation)
{
    Fixture fixture;
    MeisoK230dDrmFailureStage expected_stage;
    MeisoK230dDrmSnapshot snapshot;

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_restore_pending(&fixture));
    switch (operation) {
    case FAKE_OP_REMOVE_FB:
        fixture.fake.remove_error = EBUSY;
        fixture.fake.remove_error_once = true;
        expected_stage = MEISO_K230D_DRM_FAILURE_REMOVE_FRAMEBUFFER;
        break;
    case FAKE_OP_UNMAP:
        fixture.fake.unmap_error = EBUSY;
        fixture.fake.unmap_error_once = true;
        expected_stage = MEISO_K230D_DRM_FAILURE_UNMAP_DUMB;
        break;
    case FAKE_OP_DESTROY:
        fixture.fake.destroy_error = EBUSY;
        fixture.fake.destroy_error_once = true;
        expected_stage = MEISO_K230D_DRM_FAILURE_DESTROY_DUMB;
        break;
    case FAKE_OP_DROP_MASTER:
        fixture.fake.drop_error = EBUSY;
        fixture.fake.drop_error_once = true;
        expected_stage = MEISO_K230D_DRM_FAILURE_DROP_MASTER;
        break;
    default:
        return false;
    }
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED);
    CHECK(snapshot.cleanup.failure_stage == expected_stage);
    CHECK(snapshot.cleanup.error == EBUSY &&
          snapshot.cleanup.retained_resources);
    if (operation == FAKE_OP_REMOVE_FB) {
        CHECK(snapshot.framebuffer_owned && snapshot.mapping_owned &&
              snapshot.dumb_owned && snapshot.master_owned &&
              snapshot.fd_owned);
    } else if (operation == FAKE_OP_UNMAP) {
        CHECK(!snapshot.framebuffer_owned && snapshot.mapping_owned &&
              snapshot.dumb_owned && snapshot.master_owned &&
              snapshot.fd_owned);
    } else if (operation == FAKE_OP_DESTROY) {
        CHECK(!snapshot.framebuffer_owned && !snapshot.mapping_owned &&
              snapshot.dumb_owned && snapshot.master_owned &&
              snapshot.fd_owned);
    } else {
        CHECK(!snapshot.framebuffer_owned && !snapshot.mapping_owned &&
              !snapshot.dumb_owned && snapshot.master_owned &&
              snapshot.fd_owned);
    }
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_COMPLETE);
    CHECK(operation_count(&fixture.fake, operation) == 2U);
    return true;
}

static bool test_release_failure_ledger_and_close_consumption(void)
{
    Fixture fixture;
    MeisoK230dDrmSnapshot snapshot;

    CHECK(run_release_failure_case(FAKE_OP_REMOVE_FB));
    CHECK(run_release_failure_case(FAKE_OP_UNMAP));
    CHECK(run_release_failure_case(FAKE_OP_DESTROY));
    CHECK(run_release_failure_case(FAKE_OP_DROP_MASTER));

    CHECK(fixture_init(&fixture));
    CHECK(drive_to_restore_pending(&fixture));
    fixture.fake.close_error = EIO;
    push_event(
        &fixture.fake, MEISO_K230D_DRM_EVENT,
        FAKE_COOKIE_RESTORE, 30U, 2U, 200U, 0
    );
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_STOPPED);
    CHECK(snapshot.cleanup.failure_stage == MEISO_K230D_DRM_FAILURE_CLOSE);
    CHECK(snapshot.cleanup.error == EIO &&
          !snapshot.cleanup.retained_resources && !snapshot.fd_owned &&
          !snapshot.master_owned && !snapshot.dumb_owned &&
          !snapshot.mapping_owned && !snapshot.framebuffer_owned);
    CHECK(fixture.fake.close_calls == 1U);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_UNCHANGED);
    CHECK(fixture.fake.close_calls == 1U);
    return true;
}

static bool test_init_and_alias_boundaries(void)
{
    Fixture fixture;
    MeisoK230dDrmConfig config;
    MeisoK230dDrmConfig alias_config;
    MeisoK230dDrmOps alias_ops;
    MeisoK230dDrmScanout *output;
    MeisoK230dDrmFrameView frame;
    GuardedFrame guarded;
    GuardedFrame guarded_before;
    MeisoK230dDrmSnapshot snapshot;
    FakeDrm fake_before;
    uint8_t before[FIXTURE_ARENA_CAPACITY];
    uint8_t mutable_pixels[8];

    fake_init(&fixture.fake);
    memset(&fixture.arena, 0x5a, sizeof(fixture.arena));
    memcpy(before, fixture.arena.bytes, sizeof(before));
    output = (MeisoK230dDrmScanout *)(uintptr_t)0x1U;
    config.device_path = "relative";
    config.ops = &FAKE_OPS;
    config.ops_context = &fixture.fake;
    CHECK(meiso_k230d_drm_scanout_init(
              fixture.arena.bytes, sizeof(fixture.arena.bytes),
              &config, &output
          ) == MEISO_K230D_DRM_INIT_INVALID_CONFIG);
    CHECK(memcmp(before, fixture.arena.bytes, sizeof(before)) == 0);
    CHECK(output == (MeisoK230dDrmScanout *)(uintptr_t)0x1U);

    fake_init(&fixture.fake);
    memset(&fixture.arena, 0x5a, sizeof(fixture.arena));
    config.device_path = "/dev/dri/card0";
    config.ops = &FAKE_OPS;
    config.ops_context = &fixture.fake;
    fake_before = fixture.fake;
    memcpy(before, fixture.arena.bytes, sizeof(before));
    CHECK(meiso_k230d_drm_scanout_init(
              fixture.arena.bytes, sizeof(fixture.arena.bytes),
              &config,
              (MeisoK230dDrmScanout **)(void *)&fixture.fake
          ) == MEISO_K230D_DRM_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(&fixture.fake, &fake_before, sizeof(fake_before)) == 0);
    CHECK(memcmp(before, fixture.arena.bytes, sizeof(before)) == 0);

    fake_init(&fixture.fake);
    memset(&fixture.arena, 0x5a, sizeof(fixture.arena));
    config.device_path = "/dev/dri/card0";
    config.ops = &FAKE_OPS;
    config.ops_context = fixture.arena.bytes;
    memcpy(before, fixture.arena.bytes, sizeof(before));
    CHECK(meiso_k230d_drm_scanout_init(
              fixture.arena.bytes, sizeof(fixture.arena.bytes),
              &config, &output
          ) == MEISO_K230D_DRM_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(before, fixture.arena.bytes, sizeof(before)) == 0);

    memset(&fixture.arena, 0x5a, sizeof(fixture.arena));
    memcpy(fixture.arena.bytes + 256U, "/dev/dri/card0", 15U);
    config.device_path = (const char *)fixture.arena.bytes + 256U;
    config.ops_context = &fixture.fake;
    memcpy(before, fixture.arena.bytes, sizeof(before));
    CHECK(meiso_k230d_drm_scanout_init(
              fixture.arena.bytes, sizeof(fixture.arena.bytes),
              &config, &output
          ) == MEISO_K230D_DRM_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(before, fixture.arena.bytes, sizeof(before)) == 0);

    memset(&fixture.arena, 0x5a, sizeof(fixture.arena));
    alias_ops = FAKE_OPS;
    memcpy(fixture.arena.bytes + 256U, &alias_ops, sizeof(alias_ops));
    config.device_path = "/dev/dri/card0";
    config.ops = (const MeisoK230dDrmOps *)(const void *)(
        fixture.arena.bytes + 256U
    );
    memcpy(before, fixture.arena.bytes, sizeof(before));
    CHECK(meiso_k230d_drm_scanout_init(
              fixture.arena.bytes, sizeof(fixture.arena.bytes),
              &config, &output
          ) == MEISO_K230D_DRM_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(before, fixture.arena.bytes, sizeof(before)) == 0);

    memset(&fixture.arena, 0x5a, sizeof(fixture.arena));
    alias_config.device_path = "/dev/dri/card0";
    alias_config.ops = &FAKE_OPS;
    alias_config.ops_context = &fixture.fake;
    memcpy(
        fixture.arena.bytes + 256U,
        &alias_config,
        sizeof(alias_config)
    );
    memcpy(before, fixture.arena.bytes, sizeof(before));
    CHECK(meiso_k230d_drm_scanout_init(
              fixture.arena.bytes, sizeof(fixture.arena.bytes),
              (const MeisoK230dDrmConfig *)(const void *)(
                  fixture.arena.bytes + 256U
              ),
              &output
          ) == MEISO_K230D_DRM_INIT_INVALID_ARGUMENT);
    CHECK(memcmp(before, fixture.arena.bytes, sizeof(before)) == 0);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    frame.bytes = fixture.arena.bytes;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_INVALID_ARGUMENT);
    CHECK(fixture.fake.log_count == 0U);
    memcpy(before, fixture.arena.bytes, sizeof(before));
    CHECK(!meiso_k230d_drm_scanout_snapshot(
        fixture.scanout,
        (MeisoK230dDrmSnapshot *)(void *)fixture.arena.bytes
    ));
    CHECK(memcmp(before, fixture.arena.bytes, sizeof(before)) == 0);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    memcpy(mutable_pixels, frame.bytes, sizeof(mutable_pixels));
    frame.bytes = mutable_pixels;
    fixture.fake.mapping_result = mutable_pixels;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_CLEANUP_FAILED);
    CHECK(memcmp(mutable_pixels, rgba_frame().bytes,
                 sizeof(mutable_pixels)) == 0);
    CHECK(operation_count(&fixture.fake, FAKE_OP_ADD_FB) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_UNMAP) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DESTROY) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DROP_MASTER) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_CLOSE) == 0U);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_TERMINAL_RETAINED &&
          snapshot.cleanup_callbacks_forbidden && snapshot.fd_owned &&
          snapshot.master_owned && snapshot.dumb_owned &&
          snapshot.mapping_owned && !snapshot.framebuffer_owned);
    CHECK(meiso_k230d_drm_scanout_cleanup_step(fixture.scanout) ==
          MEISO_K230D_DRM_CLEANUP_RELEASE_FAILED);
    CHECK(operation_count(&fixture.fake, FAKE_OP_UNMAP) == 0U);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.mapping_result = fixture.arena.bytes;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_CLEANUP_FAILED);
    CHECK(operation_count(&fixture.fake, FAKE_OP_ADD_FB) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_UNMAP) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DESTROY) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DROP_MASTER) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_CLOSE) == 0U);

    CHECK(fixture_init(&fixture));
    memset(&guarded, 0x6b, sizeof(guarded));
    guarded.frame = rgba_frame();
    guarded_before = guarded;
    fixture.fake.mapping_result = &guarded.frame;
    CHECK(meiso_k230d_drm_scanout_begin(
              fixture.scanout, &guarded.frame
          ) == MEISO_K230D_DRM_BEGIN_CLEANUP_FAILED);
    CHECK(memcmp(&guarded, &guarded_before, sizeof(guarded)) == 0);
    CHECK(operation_count(&fixture.fake, FAKE_OP_ADD_FB) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_UNMAP) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DESTROY) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DROP_MASTER) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_CLOSE) == 0U);

    CHECK(fixture_init(&fixture));
    frame = rgba_frame();
    fixture.fake.mapping_result = &fixture.fake;
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_CLEANUP_FAILED);
    CHECK(operation_count(&fixture.fake, FAKE_OP_ADD_FB) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_UNMAP) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DESTROY) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_DROP_MASTER) == 0U);
    CHECK(operation_count(&fixture.fake, FAKE_OP_CLOSE) == 0U);
    return true;
}

static bool test_snapshot_output_alias_boundaries(void)
{
    Fixture fixture;
    MeisoK230dDrmFrameView frame = rgba_frame();
    MeisoK230dDrmSnapshot snapshot;
    uint8_t mapping_before[FIXTURE_MAPPING_CAPACITY];
    FakeDrm fake_before;

    CHECK(sizeof(snapshot) <= sizeof(fixture.mapping));
    CHECK(fixture_init(&fixture));
    CHECK(meiso_k230d_drm_scanout_begin(fixture.scanout, &frame) ==
          MEISO_K230D_DRM_BEGIN_PRESENT_PENDING);
    memcpy(mapping_before, fixture.mapping, sizeof(mapping_before));
    CHECK(!meiso_k230d_drm_scanout_snapshot(
        fixture.scanout,
        (MeisoK230dDrmSnapshot *)(void *)fixture.mapping
    ));
    CHECK(memcmp(mapping_before, fixture.mapping,
                 sizeof(mapping_before)) == 0);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase == MEISO_K230D_DRM_PHASE_PRESENT_PENDING &&
          snapshot.mapping_owned);

    fake_before = fixture.fake;
    CHECK(!meiso_k230d_drm_scanout_snapshot(
        fixture.scanout,
        (MeisoK230dDrmSnapshot *)(void *)&fixture.fake
    ));
    CHECK(memcmp(&fake_before, &fixture.fake, sizeof(fake_before)) == 0);

    CHECK(meiso_k230d_drm_scanout_declare_present_outcome_unknown(
              fixture.scanout, ETIMEDOUT
          ));
    memcpy(mapping_before, fixture.mapping, sizeof(mapping_before));
    CHECK(!meiso_k230d_drm_scanout_snapshot(
        fixture.scanout,
        (MeisoK230dDrmSnapshot *)(void *)fixture.mapping
    ));
    CHECK(memcmp(mapping_before, fixture.mapping,
                 sizeof(mapping_before)) == 0);
    CHECK(meiso_k230d_drm_scanout_snapshot(fixture.scanout, &snapshot));
    CHECK(snapshot.phase ==
          MEISO_K230D_DRM_PHASE_PRESENT_OUTCOME_UNKNOWN);
    return true;
}

static bool test_private_sha256_literal(void)
{
    static const uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
    };
    uint8_t actual[32];
    meiso_k230d_drm_private_sha256(
        (const uint8_t *)"abc", 3U, actual
    );
    CHECK(memcmp(actual, expected, sizeof(expected)) == 0);
    return true;
}

static bool test_topology_evaluator_and_precedence(void)
{
    FakeDrm fake;
    MeisoK230dDrmTopologyAdmission admission;
    MeisoK230dDrmTopologyAdmission before;

    fake_init(&fake);
    memset(&admission, 0xa5, sizeof(admission));
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_ELIGIBLE);
    CHECK(admission.topology_hash_valid && admission.route_selected &&
          admission.plane_selected &&
          admission.reason == MEISO_K230D_DRM_ADMISSION_NONE);
    CHECK(admission.connector_index == 0U && admission.crtc_index == 0U &&
          admission.plane_index == 1U &&
          admission.minimum_output_bytes == 32U);

    fake_init(&fake);
    fake.topology.connector_count = MEISO_K230D_DRM_MAX_CONNECTORS + 1U;
    fake.topology.driver_name[0] = '\0';
    fake.topology.page_flip_timestamps_monotonic = false;
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_BLOCKED);
    CHECK(admission.reason ==
          MEISO_K230D_DRM_ADMISSION_TOPOLOGY_CAPACITY);
    CHECK(!admission.topology_hash_valid && !admission.route_selected &&
          !admission.plane_selected);

    fake_init(&fake);
    fake.topology.driver_name[0] = '\0';
    fake.topology.page_flip_timestamps_monotonic = false;
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_BLOCKED);
    CHECK(admission.reason ==
          MEISO_K230D_DRM_ADMISSION_TOPOLOGY_NONCANONICAL);

    fake_init(&fake);
    fake.topology.page_flip_timestamps_monotonic = false;
    fake.topology.connectors[0].connected = false;
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_BLOCKED);
    CHECK(admission.reason ==
          MEISO_K230D_DRM_ADMISSION_PAGE_FLIP_CLOCK_UNPROVABLE);
    CHECK(admission.topology_hash_valid && !admission.route_selected);

    fake_init(&fake);
    fake.topology.connectors[0].connected = false;
    fake.topology.planes[1].supports_rgb565 = false;
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_BLOCKED);
    CHECK(admission.reason == MEISO_K230D_DRM_ADMISSION_NO_ACTIVE_ROUTE);

    fake_init(&fake);
    fake.topology.planes[1].supports_rgb565 = false;
    fake.topology.crtcs[0].mode.hdisplay =
        MEISO_K230D_DRM_MAX_DIMENSION + 1U;
    fake.topology.crtcs[0].mode.hsync_start =
        fake.topology.crtcs[0].mode.hdisplay;
    fake.topology.crtcs[0].mode.hsync_end =
        fake.topology.crtcs[0].mode.hdisplay;
    fake.topology.crtcs[0].mode.htotal =
        fake.topology.crtcs[0].mode.hdisplay;
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_BLOCKED);
    CHECK(admission.reason ==
          MEISO_K230D_DRM_ADMISSION_NO_UNUSED_RGB565_OVERLAY);
    CHECK(admission.route_selected && !admission.plane_selected);

    fake_init(&fake);
    fake.topology.planes[1].zpos_present = false;
    fake.topology.planes[1].zpos_immutable = false;
    fake.topology.planes[1].zpos = 0;
    fake.topology.crtcs[0].mode.hdisplay =
        MEISO_K230D_DRM_MAX_DIMENSION + 1U;
    fake.topology.crtcs[0].mode.hsync_start =
        fake.topology.crtcs[0].mode.hdisplay;
    fake.topology.crtcs[0].mode.hsync_end =
        fake.topology.crtcs[0].mode.hdisplay;
    fake.topology.crtcs[0].mode.htotal =
        fake.topology.crtcs[0].mode.hdisplay;
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_BLOCKED);
    CHECK(admission.reason == MEISO_K230D_DRM_ADMISSION_ZPOS_UNPROVABLE);

    fake_init(&fake);
    fake.topology.crtcs[0].mode.hdisplay =
        MEISO_K230D_DRM_MAX_DIMENSION + 1U;
    fake.topology.crtcs[0].mode.hsync_start =
        fake.topology.crtcs[0].mode.hdisplay;
    fake.topology.crtcs[0].mode.hsync_end =
        fake.topology.crtcs[0].mode.hdisplay;
    fake.topology.crtcs[0].mode.htotal =
        fake.topology.crtcs[0].mode.hdisplay;
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, &admission
          ) == MEISO_K230D_DRM_TOPOLOGY_BLOCKED);
    CHECK(admission.reason == MEISO_K230D_DRM_ADMISSION_MODE_ENVELOPE);
    CHECK(admission.route_selected && admission.plane_selected);

    memset(&admission, 0x6d, sizeof(admission));
    before = admission;
    CHECK(meiso_k230d_drm_private_evaluate_topology(NULL, &admission) ==
          MEISO_K230D_DRM_TOPOLOGY_INVALID_ARGUMENT);
    CHECK(memcmp(&admission, &before, sizeof(admission)) == 0);
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology, NULL
          ) == MEISO_K230D_DRM_TOPOLOGY_INVALID_ARGUMENT);
    CHECK(meiso_k230d_drm_private_evaluate_topology(
              &fake.topology,
              (MeisoK230dDrmTopologyAdmission *)(void *)&fake.topology
          ) == MEISO_K230D_DRM_TOPOLOGY_INVALID_ARGUMENT);
    return true;
}

static bool test_crtc_mask_remap(void)
{
    uint32_t resource_ids[2] = {30U, 20U};
    uint32_t canonical_ids[2] = {20U, 30U};
    uint32_t output = 0xa5a5a5a5U;
    uint32_t resource_before[2];

    CHECK(meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 0U, &output
    ));
    CHECK(output == 0U);
    CHECK(meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 1U, &output
    ));
    CHECK(output == 2U);
    CHECK(meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 2U, &output
    ));
    CHECK(output == 1U);
    CHECK(meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 3U, &output
    ));
    CHECK(output == 3U);

    output = 0xa5a5a5a5U;
    CHECK(!meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 4U, &output
    ));
    CHECK(output == 0xa5a5a5a5U);
    resource_ids[1] = 30U;
    CHECK(!meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 1U, &output
    ));
    CHECK(output == 0xa5a5a5a5U);
    resource_ids[1] = 40U;
    CHECK(!meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 1U, &output
    ));
    CHECK(output == 0xa5a5a5a5U);
    resource_ids[1] = 20U;
    canonical_ids[0] = 30U;
    canonical_ids[1] = 20U;
    CHECK(!meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 1U, &output
    ));
    CHECK(output == 0xa5a5a5a5U);

    canonical_ids[0] = 20U;
    canonical_ids[1] = 30U;
    memcpy(resource_before, resource_ids, sizeof(resource_before));
    CHECK(!meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 2U, 1U, &resource_ids[0]
    ));
    CHECK(memcmp(resource_ids, resource_before, sizeof(resource_ids)) == 0);
    CHECK(!meiso_k230d_drm_private_remap_crtc_mask(
        resource_ids, 2U, canonical_ids, 1U, 1U, &output
    ));
    CHECK(output == 0xa5a5a5a5U);
    return true;
}

int main(void)
{
    if (!test_successful_rgba_present_and_restore() ||
        !test_restore_preserves_nonzero_detached_geometry() ||
        !test_rgb565_be_conversion() ||
        !test_input_and_topology_admission() ||
        !test_topology_canonical_contract() ||
        !test_plane_safety_admission_matrix() ||
        !test_input_mode_and_dumb_bounds() ||
        !test_acquisition_failure_release_matrix() ||
        !test_commit_dispositions_and_event_unknown() ||
        !test_restore_unknown_readback_and_release_retry() ||
        !test_restore_rejected_and_definite_retry() ||
        !test_plane_readback_equality_matrix() ||
        !test_readback_error_retains_every_resource() ||
        !test_release_failure_ledger_and_close_consumption() ||
        !test_init_and_alias_boundaries() ||
        !test_snapshot_output_alias_boundaries() ||
        !test_private_sha256_literal() ||
        !test_topology_evaluator_and_precedence() ||
        !test_crtc_mask_remap()) {
        return 1;
    }
    (void)printf("K230D_DRM_SCANOUT_POLICY_TEST_PASSED\n");
    return 0;
}
