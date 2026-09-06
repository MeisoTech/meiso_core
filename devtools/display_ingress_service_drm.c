#define _POSIX_C_SOURCE 200809L
#include "display_ingress_simulated_service_private.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

/* One process, one frame. This is the native end of the bounded lab service. */
static const char *names[] = {"FB_ID", "CRTC_ID", "SRC_X", "SRC_Y",
    "SRC_W", "SRC_H", "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"};
enum { NPROPS = sizeof(names) / sizeof(names[0]), MAX_BUFFER = 5 * 1024 * 1024 };
static struct {
    int fd;
    uint32_t plane, crtc, fb, width, height;
    uint32_t ids[NPROPS];
    uint64_t before[NPROPS];
    struct drm_mode_create_dumb dumb;
    void *map;
    uint64_t flip_ns;
    bool submitted, event_seen, bad_event;
} display = {.fd = -1, .map = MAP_FAILED};
static volatile sig_atomic_t interrupted;
static void stop(int sig) { (void)sig; interrupted = 1; }

static bool read_properties(uint32_t plane, uint32_t *ids, uint64_t *values,
                            bool *overlay)
{
    drmModeObjectProperties *ps = drmModeObjectGetProperties(display.fd, plane, DRM_MODE_OBJECT_PLANE);
    unsigned found = 0;
    if (!ps) return false;
    for (uint32_t j = 0; j < ps->count_props; j++) {
        drmModePropertyRes *p = drmModeGetProperty(display.fd, ps->props[j]);
        if (!p) { drmModeFreeObjectProperties(ps); return false; }
        if (overlay && !strcmp(p->name, "type"))
            *overlay = (p->flags & DRM_MODE_PROP_IMMUTABLE) && ps->prop_values[j] == DRM_PLANE_TYPE_OVERLAY;
        for (unsigned i = 0; i < NPROPS; i++) {
            if (!strcmp(p->name, names[i])) {
                ids[i] = p->prop_id; values[i] = ps->prop_values[j]; found++;
            }
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(ps);
    return found == NPROPS;
}

static bool commit(const uint64_t *values, uint32_t flags, void *cookie)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    bool ok = false;
    if (!req) return false;
    for (unsigned i = 0; i < NPROPS; i++)
        if (drmModeAtomicAddProperty(req, display.plane, display.ids[i], values[i]) < 0) goto out;
    ok = drmModeAtomicCommit(display.fd, req, flags, cookie) == 0;
out:
    drmModeAtomicFree(req);
    return ok;
}

bool service_drm_open(Service *service)
{
    if (service->role == MEISO_SESSION_ROLE_HOST) return true;
    drmModeRes *res = NULL;
    drmModePlaneRes *planes = NULL;
    drmModeCrtc *crtc = NULL;
    bool ok = false;
    uint64_t cap = 0;
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL); sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "drm admission failed: %s (%d)\n", #x, errno); goto out; } } while (0)
    REQUIRE(service->drm_device_path != NULL);
    display.fd = open(service->drm_device_path, O_RDWR | O_CLOEXEC);
    REQUIRE(display.fd >= 0 && drmSetMaster(display.fd) == 0);
    REQUIRE(drmSetClientCap(display.fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0);
    REQUIRE(drmGetCap(display.fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) == 0 && cap);
    REQUIRE((res = drmModeGetResources(display.fd)) != NULL && res->count_crtcs == 1);
    display.crtc = res->crtcs[0];
    REQUIRE((crtc = drmModeGetCrtc(display.fd, display.crtc)) != NULL && crtc->mode_valid);
    drmModeObjectProperties *crtc_props = drmModeObjectGetProperties(display.fd, display.crtc, DRM_MODE_OBJECT_CRTC);
    REQUIRE(crtc_props != NULL);
    bool active = false;
    for (uint32_t i = 0; i < crtc_props->count_props; i++) {
        drmModePropertyRes *p = drmModeGetProperty(display.fd, crtc_props->props[i]);
        if (p) {
            if (!strcmp(p->name, "ACTIVE")) active = crtc_props->prop_values[i] == 1;
            drmModeFreeProperty(p);
        }
    }
    drmModeFreeObjectProperties(crtc_props);
    REQUIRE(active);
    display.width = crtc->mode.hdisplay; display.height = crtc->mode.vdisplay;
    REQUIRE(display.width && display.height && display.width <= 4096 && display.height <= 4096);
    REQUIRE((uint64_t)display.width * display.height * 2 <= MAX_BUFFER);
    unsigned connected = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(display.fd, res->connectors[i]);
        REQUIRE(c != NULL);
        drmModeEncoder *enc = c->encoder_id ? drmModeGetEncoder(display.fd, c->encoder_id) : NULL;
        if (c->connection == DRM_MODE_CONNECTED && enc && enc->crtc_id == display.crtc) {
            connected++;
            if (c->connector_type == DRM_MODE_CONNECTOR_DSI)
                service->presentation.connector_kind = MEISO_DISPLAY_INGRESS_CONNECTOR_DSI;
            else if (c->connector_type == DRM_MODE_CONNECTOR_HDMIA || c->connector_type == DRM_MODE_CONNECTOR_HDMIB)
                service->presentation.connector_kind = MEISO_DISPLAY_INGRESS_CONNECTOR_HDMI;
        }
        if (enc) drmModeFreeEncoder(enc);
        drmModeFreeConnector(c);
    }
    REQUIRE(connected == 1 && service->presentation.connector_kind != MEISO_DISPLAY_INGRESS_CONNECTOR_UNSPECIFIED);
    REQUIRE((planes = drmModeGetPlaneResources(display.fd)) != NULL);
    for (uint32_t i = 0; i < planes->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(display.fd, planes->planes[i]);
        REQUIRE(p != NULL);
        bool busy = p->fb_id || p->crtc_id, supported = false;
        uint32_t id = p->plane_id;
        if (p->possible_crtcs & 1U)
            for (uint32_t j = 0; j < p->count_formats; j++)
                if (p->formats[j] == DRM_FORMAT_RGB565) supported = true;
        drmModeFreePlane(p);
        REQUIRE(!busy);
        if (!display.plane && supported) {
            bool overlay = false;
            REQUIRE(read_properties(id, display.ids, display.before, &overlay));
            if (overlay) display.plane = id;
        }
    }
    REQUIRE(display.plane != 0 && !interrupted);
    display.dumb.width = display.width; display.dumb.height = display.height; display.dumb.bpp = 16;
    REQUIRE(drmIoctl(display.fd, DRM_IOCTL_MODE_CREATE_DUMB, &display.dumb) == 0);
    REQUIRE(display.dumb.size <= MAX_BUFFER && display.dumb.pitch >= display.width * 2 &&
            display.dumb.size >= (uint64_t)display.dumb.pitch * display.height);
    struct drm_mode_map_dumb m = {.handle = display.dumb.handle};
    REQUIRE(drmIoctl(display.fd, DRM_IOCTL_MODE_MAP_DUMB, &m) == 0);
    display.map = mmap(NULL, display.dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, display.fd, m.offset);
    REQUIRE(display.map != MAP_FAILED);
    uint32_t handles[4] = {display.dumb.handle}, pitches[4] = {display.dumb.pitch}, offsets[4] = {0};
    REQUIRE(drmModeAddFB2(display.fd, display.width, display.height, DRM_FORMAT_RGB565,
                        handles, pitches, offsets, &display.fb, 0) == 0);
    service->presentation.output_width = display.width;
    service->presentation.output_height = display.height;
    service->presentation.evidence_class = MEISO_DISPLAY_INGRESS_EVIDENCE_DRM_KMS_PAGE_FLIP;
    fprintf(stderr, "DRM_READY output=%ux%u buffer_bytes=%llu\n", display.width, display.height,
            (unsigned long long)display.dumb.size);
    ok = true;
out:
    if (crtc) drmModeFreeCrtc(crtc);
    if (planes) drmModeFreePlaneResources(planes);
    if (res) drmModeFreeResources(res);
    return ok || service_fail(service, "display", "admission");
#undef REQUIRE
}

static void page_flip(int fd, unsigned seq, unsigned sec, unsigned usec,
                      unsigned crtc, void *cookie)
{
    (void)seq;
    if (fd != display.fd || crtc != display.crtc || cookie != &display || display.event_seen) {
        display.bad_event = true; return;
    }
    display.event_seen = true;
    display.flip_ns = (uint64_t)sec * UINT64_C(1000000000) + (uint64_t)usec * 1000U;
}

bool service_drm_present(Service *service, MeisoByteView rgba)
{
    if (display.map == MAP_FAILED || rgba.size != SERVICE_FRAME_BYTES || !rgba.data || interrupted)
        return service_fail(service, "display", "input");
    memset(display.map, 0, display.dumb.size);
    for (uint32_t y = 0; y < display.height; y++) {
        uint16_t *row = (uint16_t *)((uint8_t *)display.map + y * display.dumb.pitch);
        for (uint32_t x = 0; x < display.width; x++) {
            size_t src = ((size_t)(y * SERVICE_FRAME_HEIGHT / display.height) * SERVICE_FRAME_WIDTH +
                          x * SERVICE_FRAME_WIDTH / display.width) * 4U;
            row[x] = (uint16_t)(((rgba.data[src] >> 3U) << 11U) |
                               ((rgba.data[src+1] >> 2U) << 5U) | (rgba.data[src+2] >> 3U));
        }
    }
    uint64_t values[NPROPS] = {display.fb, display.crtc, 0, 0,
        (uint64_t)display.width << 16U, (uint64_t)display.height << 16U,
        0, 0, display.width, display.height};
    if (!commit(values, DRM_MODE_ATOMIC_TEST_ONLY, NULL) ||
        !commit(values, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, &display))
        return service_fail(service, "display", "commit");
    display.submitted = true;
    drmEventContext events = {.version = 3, .page_flip_handler2 = page_flip};
    uint64_t deadline = service_deadline_after_ms(2000U);
    while (!display.event_seen && !display.bad_event && !interrupted && !service_deadline_expired(deadline)) {
        struct pollfd p = {.fd = display.fd, .events = POLLIN};
        int n = poll(&p, 1, 20);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if ((p.revents & POLLIN) && drmHandleEvent(display.fd, &events) != 0) break;
    }
    uint32_t ids[NPROPS]; uint64_t after[NPROPS];
    if (!display.event_seen || display.bad_event || interrupted || !display.flip_ns ||
        !read_properties(display.plane, ids, after, NULL) || memcmp(values, after, sizeof(after)))
        return service_fail(service, "display", "page_flip");
    service->presentation_time = (MeisoTimePoint){SERVICE_EVIDENCE_CLOCK_EPOCH, display.flip_ns};
    fprintf(stderr, "DRM_PAGE_FLIP frame=%llu output=%ux%u\n",
            (unsigned long long)service->frame_sequence, display.width, display.height);
    return true;
}

bool service_drm_close(Service *service)
{
    bool ok = true;
    if (display.submitted) {
        uint32_t ids[NPROPS]; uint64_t after[NPROPS];
        ok = commit(display.before, 0, NULL) && read_properties(display.plane, ids, after, NULL) &&
            memcmp(display.before, after, sizeof(after)) == 0;
    }
    if (display.fb && drmModeRmFB(display.fd, display.fb)) ok = false;
    if (display.map != MAP_FAILED && munmap(display.map, display.dumb.size)) ok = false;
    if (display.dumb.handle) {
        struct drm_mode_destroy_dumb d = {.handle = display.dumb.handle};
        if (drmIoctl(display.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d)) ok = false;
    }
    if (display.fd >= 0) close(display.fd);
    if (service->role == MEISO_SESSION_ROLE_EDGE)
        fprintf(stderr, "DRM_CLEANUP restored=%s\n", ok ? "true" : "false");
    service->display_closed = ok;
    return ok || service_fail(service, "display", "cleanup");
}
