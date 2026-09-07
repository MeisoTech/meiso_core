#define _POSIX_C_SOURCE 200809L
#include "display_ingress_video_backend.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#ifndef V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT
#define V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT 0x10000000U
#endif
#ifndef V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC
#define V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC 0x20000000U
#endif
enum { INPUT_BUFFERS = 2, CAPTURE_BUFFERS = 7, AU_LIMIT = 128 * 1024,
       INPUT_BYTES = 1024 * 1024, DUMMY_BYTES = 4096, NPROPS = 10,
       OVERLAY_BYTES = 256 * 1024, CAPTURE_POOL_BYTES = 10 * 1024 * 1024 };
enum state { WAIT_SOURCE, READY, FAILED };

struct surface { int fd; uint32_t handle; void *map; size_t bytes; };
struct flip { int pending; int seen; uint64_t ns; uint64_t sequence; int capture_index;
              uint32_t fb, handles[2]; };
struct MeisoIngressVideo {
    int video_fd, drm_fd, output_streaming, capture_streaming, source_changed;
    int output_buffers_requested, capture_buffers_requested, source_subscribed;
    enum state state;
    uint32_t width, height, output_w, output_h, ystride, uvstride, crtc, plane, prop_ids[NPROPS];
    uint64_t before[NPROPS];
    struct { uint32_t plane, props[NPROPS]; uint64_t before[NPROPS]; } right;
    MeisoIngressVideoConnectorKind connector;
    struct surface input[INPUT_BUFFERS], dummy[2], capture[CAPTURE_BUFFERS][2];
    uint32_t input_v4l_length[INPUT_BUFFERS], dummy_v4l_length[2], capture_v4l_length[CAPTURE_BUFFERS][2];
    int input_busy[INPUT_BUFFERS], capture_count, held_capture;
    uint32_t shown_fb, shown_handles[2];
    struct flip flip;
    struct {
        uint32_t plane, props[NPROPS], fb[2], width[2], height[2];
        uint64_t before[NPROPS], sequence, last_sequence, ns;
        struct surface surfaces[2];
        int front, target, pending, seen;
    } overlay;
};

static const char *const property_names[NPROPS] = {
    "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
    "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"
};

static int destroy_surface(int drm_fd, struct surface *s)
{
    int rc = 0;
    if (s->map && munmap(s->map, s->bytes)) rc = -1;
    if (s->handle && drm_fd >= 0) {
        struct drm_mode_destroy_dumb d = { .handle = s->handle };
        if (ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d)) rc = -1;
    }
    if (s->fd >= 0 && close(s->fd)) rc = -1;
    memset(s, 0, sizeof(*s)); s->fd = -1;
    return rc;
}

static int make_surface(int drm_fd, size_t bytes, int map, struct surface *s)
{
    struct drm_mode_create_dumb create = {0};
    struct drm_prime_handle prime = {0};
    memset(s, 0, sizeof(*s)); s->fd = -1;
    if (!bytes || bytes > 8U * 1024U * 1024U) { errno = EINVAL; return -1; }
    create.width = 4096; create.height = (bytes + 4095) / 4096; create.bpp = 8;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) return -1;
    prime.handle = create.handle; prime.flags = DRM_CLOEXEC | DRM_RDWR;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime)) {
        struct drm_mode_destroy_dumb d = { .handle = create.handle };
        (void)ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d); return -1;
    }
    s->fd = prime.fd; s->handle = create.handle; s->bytes = create.size;
    if (map) {
        struct drm_mode_map_dumb m = { .handle = create.handle };
        if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) { (void)destroy_surface(drm_fd, s); return -1; }
        s->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, m.offset);
        if (s->map == MAP_FAILED) { s->map = NULL; (void)destroy_surface(drm_fd, s); return -1; }
    } else {
        struct drm_mode_destroy_dumb d = { .handle = create.handle };
        (void)ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        s->handle = 0;
    }
    return 0;
}

static int read_properties_at(int fd, uint32_t plane, uint32_t *ids, uint64_t *values)
{
    drmModeObjectProperties *ps = drmModeObjectGetProperties(fd, plane, DRM_MODE_OBJECT_PLANE);
    unsigned found = 0;
    if (!ps) return -1;
    for (uint32_t i = 0; i < ps->count_props; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(fd, ps->props[i]);
        if (!p) continue;
        for (unsigned j = 0; j < NPROPS; ++j) if (!strcmp(p->name, property_names[j])) {
            ids[j] = p->prop_id; values[j] = ps->prop_values[i]; ++found;
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(ps);
    return found == NPROPS ? 0 : -1;
}

static int atomic_plane_commit(int fd, uint32_t plane, const uint32_t *ids,
                               const uint64_t *values, uint32_t flags, void *cookie)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return -1;
    for (unsigned i = 0; i < NPROPS; ++i)
        if (drmModeAtomicAddProperty(req, plane, ids[i], values[i]) < 0) {
            drmModeAtomicFree(req); return -1;
        }
    int rc = drmModeAtomicCommit(fd, req, flags, cookie);
    drmModeAtomicFree(req); return rc;
}

static int atomic_commit(struct MeisoIngressVideo *v, const uint64_t *left,
                         const uint64_t *right, uint32_t flags, void *cookie)
{
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    if (!req) return -1;
    const uint32_t planes[2] = {v->plane, v->right.plane};
    const uint32_t *ids[2] = {v->prop_ids, v->right.props};
    const uint64_t *values[2] = {left, right};
    int rc = -1;
    for (unsigned p = 0; p < 2; ++p) if (planes[p])
        for (unsigned i = 0; i < NPROPS; ++i)
            if (drmModeAtomicAddProperty(req, planes[p], ids[p][i], values[p][i]) < 0) goto out;
    rc = drmModeAtomicCommit(v->drm_fd, req, flags, cookie);
out:
    drmModeAtomicFree(req); return rc;
}

static void page_flip(int fd, unsigned sequence, unsigned sec, unsigned usec,
                      unsigned crtc, void *cookie)
{
    struct MeisoIngressVideo *v = cookie;
    (void)sequence;
    if (v && fd == v->drm_fd && crtc == v->crtc && v->overlay.pending && !v->overlay.seen) {
        v->overlay.seen = 1;
        v->overlay.ns = (uint64_t)sec * UINT64_C(1000000000) + (uint64_t)usec * 1000;
        return;
    }
    if (v && fd == v->drm_fd && crtc == v->crtc && v->flip.pending && !v->flip.seen) {
        v->flip.seen = 1;
        v->flip.ns = (uint64_t)sec * UINT64_C(1000000000) + (uint64_t)usec * 1000;
    }
}

static int release_fb(struct MeisoIngressVideo *v, uint32_t fb, uint32_t handles[2])
{
    int rc = 0;
    if (fb && drmModeRmFB(v->drm_fd, fb)) rc = -1;
    for (unsigned i = 0; i < 2; ++i) if (handles[i] && (!i || handles[i] != handles[0])) {
        struct drm_gem_close close_arg = { .handle = handles[i] };
        if (drmIoctl(v->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg)) rc = -1;
    }
    return rc;
}

static int queue_capture(struct MeisoIngressVideo *v, unsigned index)
{
    struct v4l2_plane p[2] = {0};
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_DMABUF, .index = index, .length = 2 };
    b.m.planes = p;
    p[0].m.fd = v->capture[index][0].fd; p[0].length = v->capture_v4l_length[index][0];
    p[1].m.fd = v->capture[index][1].fd; p[1].length = v->capture_v4l_length[index][1];
    return ioctl(v->video_fd, VIDIOC_QBUF, &b);
}

static int queue_dummy(struct MeisoIngressVideo *v)
{
    struct v4l2_plane p[2] = {0};
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_DMABUF, .index = 0, .length = 2 };
    b.m.planes = p;
    p[0].m.fd = v->dummy[0].fd; p[0].length = v->dummy_v4l_length[0];
    p[1].m.fd = v->dummy[1].fd; p[1].length = v->dummy_v4l_length[1];
    return ioctl(v->video_fd, VIDIOC_QBUF, &b);
}

static int request_buffers(struct MeisoIngressVideo *v, enum v4l2_buf_type type, unsigned count)
{
    struct v4l2_requestbuffers r = { .count = count, .type = type, .memory = V4L2_MEMORY_DMABUF };
    if (ioctl(v->video_fd, VIDIOC_REQBUFS, &r)) return -1;
    if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT) v->output_buffers_requested = r.count != 0;
    else if (type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) v->capture_buffers_requested = r.count != 0;
    return count && r.count != count ? (errno = ENOMEM, -1) : 0;
}

static int sync_for_cpu_write(int fd, int start)
{
    struct dma_buf_sync sync = { .flags = (uint64_t)DMA_BUF_SYNC_WRITE |
        (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) };
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static int query_buffer(struct MeisoIngressVideo *v, enum v4l2_buf_type type, unsigned index,
                        uint32_t *single_length, uint32_t plane_length[2])
{
    struct v4l2_plane p[2] = {0};
    struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_DMABUF, .index = index };
    if (V4L2_TYPE_IS_MULTIPLANAR(type)) { b.length = 2; b.m.planes = p; }
    if (ioctl(v->video_fd, VIDIOC_QUERYBUF, &b)) return -1;
    if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
        if (b.length != 2 || !p[0].length || !p[1].length) { errno = EPROTO; return -1; }
        plane_length[0] = p[0].length; plane_length[1] = p[1].length;
    } else {
        if (!b.length) { errno = EPROTO; return -1; }
        *single_length = b.length;
    }
    return 0;
}

static int stream(struct MeisoIngressVideo *v, enum v4l2_buf_type type, int on)
{
    return ioctl(v->video_fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type);
}

static int setup_display(struct MeisoIngressVideo *v)
{
    drmModeRes *res = drmModeGetResources(v->drm_fd);
    drmModeCrtc *crtc = NULL;
    drmModePlaneRes *planes = NULL;
    if (!res || res->count_crtcs != 1) goto fail;
    crtc = drmModeGetCrtc(v->drm_fd, res->crtcs[0]);
    if (!crtc || !crtc->mode_valid) goto fail;
    v->crtc = crtc->crtc_id; v->output_w = crtc->mode.hdisplay; v->output_h = crtc->mode.vdisplay;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *c = drmModeGetConnector(v->drm_fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED) {
            if (c->connector_type == DRM_MODE_CONNECTOR_DSI) v->connector = MEISO_INGRESS_VIDEO_CONNECTOR_DSI;
            else if (c->connector_type == DRM_MODE_CONNECTOR_HDMIA || c->connector_type == DRM_MODE_CONNECTOR_HDMIB)
                v->connector = MEISO_INGRESS_VIDEO_CONNECTOR_HDMI;
        }
        if (c) drmModeFreeConnector(c);
    }
    planes = drmModeGetPlaneResources(v->drm_fd);
    if (!planes) goto fail;
    for (uint32_t i = 0; i < planes->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(v->drm_fd, planes->planes[i]);
        if (!p) goto fail;
        if (p->fb_id || p->crtc_id) { drmModeFreePlane(p); goto fail; }
        if (!v->plane && (p->possible_crtcs & 1U))
            for (uint32_t j = 0; j < p->count_formats; ++j)
                if (p->formats[j] == DRM_FORMAT_NV12) v->plane = p->plane_id;
        drmModeFreePlane(p);
    }
    if (!v->plane || read_properties_at(v->drm_fd, v->plane, v->prop_ids, v->before)) goto fail;
    drmModeFreePlaneResources(planes); drmModeFreeCrtc(crtc); drmModeFreeResources(res); return 0;
fail:
    if (planes) drmModeFreePlaneResources(planes);
    if (crtc) drmModeFreeCrtc(crtc);
    if (res) drmModeFreeResources(res);
    return -1;
}

static int reconfigure_capture(struct MeisoIngressVideo *v)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_format fmt = { .type = type };
    if (v->capture_streaming && stream(v, type, 0)) return -1;
    v->capture_streaming = 0;
    if (request_buffers(v, type, 0)) return -1;
    if (destroy_surface(v->drm_fd, &v->dummy[0]) || destroy_surface(v->drm_fd, &v->dummy[1])) return -1;
    if (ioctl(v->video_fd, VIDIOC_G_FMT, &fmt)) return -1;
    if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12 || fmt.fmt.pix_mp.num_planes != 2 ||
        fmt.fmt.pix_mp.width != v->width || fmt.fmt.pix_mp.height != v->height) { errno = EPROTO; return -1; }
    v->ystride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    v->uvstride = fmt.fmt.pix_mp.plane_fmt[1].bytesperline;
    if (v->ystride < v->width || v->uvstride < v->width) { errno = EPROTO; return -1; }
    struct v4l2_control minimum = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    if (ioctl(v->video_fd, VIDIOC_G_CTRL, &minimum)) return -1;
    /* Keep one surface for the shown frame beyond the decoder minimum.
     * During a pending flip the decoder may wait; the old shown surface is
     * returned as soon as that flip completes, rather than reserving another. */
    if (minimum.value < 1 || minimum.value > CAPTURE_BUFFERS - 1) { errno = ENOSPC; return -1; }
    const unsigned count = (unsigned)minimum.value + 1;
    if (request_buffers(v, type, count)) return -1;
    size_t pool_bytes = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (query_buffer(v, type, i, NULL, v->capture_v4l_length[i])) return -1;
        for (unsigned p = 0; p < 2; ++p)
            pool_bytes += ((size_t)v->capture_v4l_length[i][p] + 4095) & ~(size_t)4095;
    }
    fprintf(stderr,"CAPTURE_BUDGET width=%u height=%u minimum=%d count=%u bytes=%zu limit=%u\n",
        v->width,v->height,minimum.value,count,pool_bytes,CAPTURE_POOL_BYTES);
    if (pool_bytes > CAPTURE_POOL_BYTES) { errno = ENOSPC; return -1; }
    for (unsigned i = 0; i < count; ++i) {
        if (make_surface(v->drm_fd, v->capture_v4l_length[i][0], 0, &v->capture[i][0]) ||
            make_surface(v->drm_fd, v->capture_v4l_length[i][1], 0, &v->capture[i][1]) ||
            queue_capture(v, i)) return -1;
    }
    v->capture_count = count;
    if (stream(v, type, 1)) return -1;
    v->capture_streaming = 1; v->state = READY;
    return 0;
}

static int service_source_change(struct MeisoIngressVideo *v)
{
    struct v4l2_event e = {0};
    int got = 0;
    while (ioctl(v->video_fd, VIDIOC_DQEVENT, &e) == 0) if (e.type == V4L2_EVENT_SOURCE_CHANGE) got = 1;
    /* This MVX build reports an empty event queue as ENOENT rather than EAGAIN. */
    if (!got && errno != EAGAIN && errno != ENOENT) return -1;
    if (!got && !v->source_changed) {
        struct v4l2_plane p[2] = {0};
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            .memory = V4L2_MEMORY_DMABUF, .length = 2 };
        b.m.planes = p;
        if (ioctl(v->video_fd, VIDIOC_DQBUF, &b) == 0) {
            if (b.flags & V4L2_BUF_FLAG_MVX_BUFFER_NEED_REALLOC) got = 1;
            else if (queue_dummy(v)) return -1;
        } else if (errno != EAGAIN) return -1;
    }
    if (got && !v->source_changed) { v->source_changed = 1; return reconfigure_capture(v); }
    return 0;
}

static int reap_output(struct MeisoIngressVideo *v)
{
    for (;;) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF };
        if (ioctl(v->video_fd, VIDIOC_DQBUF, &b)) return errno == EAGAIN ? 0 : -1;
        if (b.index >= INPUT_BUFFERS) { errno = EPROTO; return -1; }
        v->input_busy[b.index] = 0;
    }
}

static int start_present(struct MeisoIngressVideo *v, unsigned capture_index, const struct v4l2_buffer *b)
{
    uint32_t handles[4] = {0}, fb = 0, offsets[4] = {0};
    uint32_t pitches[4] = { v->ystride, v->uvstride };
    if (!pitches[0] || !pitches[1] || drmPrimeFDToHandle(v->drm_fd, v->capture[capture_index][0].fd, &handles[0]) ||
        drmPrimeFDToHandle(v->drm_fd, v->capture[capture_index][1].fd, &handles[1]) ||
        drmModeAddFB2(v->drm_fd, v->width, v->height, DRM_FORMAT_NV12, handles, pitches, offsets, &fb, 0)) goto fail;
    const uint32_t w = v->right.plane ? v->width / 2 : v->width;
    const uint32_t x = v->right.plane ? (v->output_w / 2 - w) / 2 : 0;
    const uint32_t y = v->right.plane ? (v->output_h - v->height) / 2 : 0;
    const uint64_t values[NPROPS] = {fb, v->crtc, 0, 0, (uint64_t)w << 16,
        (uint64_t)v->height << 16, x, y, w, v->height};
    const uint64_t right[NPROPS] = {fb, v->crtc, (uint64_t)w << 16, 0, (uint64_t)w << 16,
        (uint64_t)v->height << 16, x + v->output_w / 2, y, w, v->height};
    if (atomic_commit(v, values, right, DRM_MODE_ATOMIC_TEST_ONLY, NULL) ||
        atomic_commit(v, values, right, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, v)) goto fail;
    v->flip.pending = 1; v->flip.seen = 0; v->flip.capture_index = (int)capture_index;
    v->flip.fb = fb; v->flip.handles[0] = handles[0]; v->flip.handles[1] = handles[1];
    v->flip.sequence = (uint64_t)b->timestamp.tv_sec * UINT64_C(1000000) + (uint32_t)b->timestamp.tv_usec;
    return 0;
fail:
    (void)release_fb(v, fb, handles); return -1;
}

static int finish_flip(struct MeisoIngressVideo *v, MeisoIngressVideoPresentation *out)
{
    if (!v->flip.seen) return 0;
    if (release_fb(v, v->shown_fb, v->shown_handles)) return -1;
    if (v->held_capture >= 0 && queue_capture(v, (unsigned)v->held_capture)) return -1;
    v->shown_fb = v->flip.fb; v->shown_handles[0] = v->flip.handles[0]; v->shown_handles[1] = v->flip.handles[1];
    v->held_capture = v->flip.capture_index;
    out->sequence = v->flip.sequence; out->page_flip_monotonic_ns = v->flip.ns;
    out->output_width = v->output_w; out->output_height = v->output_h; out->connector_kind = v->connector;
    memset(&v->flip, 0, sizeof(v->flip)); return 1;
}

MeisoIngressVideo *meiso_ingress_video_open(const char *video_path, const char *drm_path,
                                             uint32_t width, uint32_t height)
{
    struct MeisoIngressVideo *v = calloc(1, sizeof(*v));
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
    struct v4l2_format capture_fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    enum v4l2_buf_type out = V4L2_BUF_TYPE_VIDEO_OUTPUT, cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (!v) { errno = ENOMEM; goto fail; }
    v->video_fd = v->drm_fd = -1; v->held_capture = -1;
    v->overlay.front = v->overlay.target = -1;
    v->overlay.surfaces[0].fd = v->overlay.surfaces[1].fd = -1;
    for (int i = 0; i < INPUT_BUFFERS; ++i) v->input[i].fd = -1;
    for (int i = 0; i < CAPTURE_BUFFERS; ++i) { v->capture[i][0].fd = -1; v->capture[i][1].fd = -1; }
    v->dummy[0].fd = v->dummy[1].fd = -1;
    if (!video_path || !drm_path || !width || !height || (width & 1) || (height & 1)) { errno = EINVAL; goto fail; }
    v->width = width; v->height = height;
    v->video_fd = open(video_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    v->drm_fd = open(drm_path, O_RDWR | O_CLOEXEC);
    if (v->video_fd < 0 || v->drm_fd < 0 || drmSetMaster(v->drm_fd) || drmSetClientCap(v->drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) || setup_display(v)) goto fail;
    /* MVX discovers dimensions from the first IDR; pre-source-change OUTPUT is 0x0. */
    fmt.fmt.pix.width = 0; fmt.fmt.pix.height = 0; fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    fmt.fmt.pix.field = V4L2_FIELD_NONE; fmt.fmt.pix.sizeimage = INPUT_BYTES;
    if (ioctl(v->video_fd, VIDIOC_S_FMT, &fmt) || request_buffers(v, out, INPUT_BUFFERS)) goto fail;
    for (int i = 0; i < INPUT_BUFFERS; ++i)
        if (query_buffer(v, out, i, &v->input_v4l_length[i], NULL) ||
            make_surface(v->drm_fd, v->input_v4l_length[i], 1, &v->input[i])) goto fail;
    /* MVX needs a one-buffer capture queue before SOURCE_CHANGE.  Keep that
     * provisional format dimensionless so its two 4 KiB DMA-BUFs are accepted;
     * the real NV12 dimensions and seven surfaces are negotiated afterwards. */
    capture_fmt.fmt.pix_mp.width = 0; capture_fmt.fmt.pix_mp.height = 0;
    capture_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12; capture_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    capture_fmt.fmt.pix_mp.num_planes = 2;
    if (ioctl(v->video_fd, VIDIOC_S_FMT, &capture_fmt) || request_buffers(v, cap, 1) ||
        query_buffer(v, cap, 0, NULL, v->dummy_v4l_length) ||
        make_surface(v->drm_fd, v->dummy_v4l_length[0], 0, &v->dummy[0]) ||
        make_surface(v->drm_fd, v->dummy_v4l_length[1], 0, &v->dummy[1]) || queue_dummy(v) || ioctl(v->video_fd, VIDIOC_SUBSCRIBE_EVENT, &sub)) goto fail;
    v->source_subscribed = 1;
    if (stream(v, out, 1)) goto fail;
    v->output_streaming = 1;
    if (stream(v, cap, 1)) goto fail;
    v->capture_streaming = 1; v->state = WAIT_SOURCE; return v;
fail:
    meiso_ingress_video_close(v); return NULL;
}

int meiso_ingress_video_set_stereo(MeisoIngressVideo *v)
{
    if (!v || v->state != WAIT_SOURCE || v->source_changed || v->right.plane ||
        v->width % 16 || v->output_w % 2 || v->width > v->output_w || v->height > v->output_h)
        return errno = EINVAL, -1;
    for (unsigned i = 0; i < INPUT_BUFFERS; ++i)
        if (v->input_busy[i]) return errno = EBUSY, -1;
    drmModePlaneRes *planes = drmModeGetPlaneResources(v->drm_fd);
    if (!planes) return -1;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < planes->count_planes && !chosen; ++i) {
        drmModePlane *p = drmModeGetPlane(v->drm_fd, planes->planes[i]);
        if (!p) break;
        int nv12 = 0, rotating = 0;
        for (uint32_t j = 0; j < p->count_formats; ++j) nv12 |= p->formats[j] == DRM_FORMAT_NV12;
        if (nv12 && p->plane_id != v->plane && !p->fb_id && !p->crtc_id && (p->possible_crtcs & 1U)) {
            drmModeObjectProperties *ps = drmModeObjectGetProperties(v->drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
            if (ps) {
                for (uint32_t j = 0; j < ps->count_props; ++j) {
                    drmModePropertyRes *prop = drmModeGetProperty(v->drm_fd, ps->props[j]);
                    if (!prop) { rotating = 1; break; }
                    rotating |= !strcmp(prop->name, "rotation");
                    drmModeFreeProperty(prop);
                }
                drmModeFreeObjectProperties(ps);
                if (!rotating) chosen = p->plane_id;
            }
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(planes);
    if (!chosen || read_properties_at(v->drm_fd, chosen, v->right.props, v->right.before))
        return errno = ENODEV, -1;
    v->right.plane = chosen;
    fprintf(stderr, "STEREO_VIDEO_LAYOUT input=%ux%u output=%ux%u eye=%ux%u x=%u,%u y=%u planes=%u,%u extra_pixel_bytes=0\n",
        v->width, v->height, v->output_w, v->output_h, v->width/2, v->height,
        (v->output_w-v->width)/4, (v->output_w-v->width)/4+v->output_w/2,
        (v->output_h-v->height)/2, v->plane, v->right.plane);
    return 0;
}

MeisoIngressVideoSubmitResult meiso_ingress_video_submit(MeisoIngressVideo *v, uint64_t sequence,
                                                          const uint8_t *au, size_t au_size)
{
    if (!v || !au || !au_size || au_size > AU_LIMIT || v->state == FAILED) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if (reap_output(v)) { v->state = FAILED; return MEISO_INGRESS_VIDEO_SUBMIT_ERROR; }
    int index = -1; for (int i = 0; i < INPUT_BUFFERS; ++i) if (!v->input_busy[i]) { index = i; break; }
    if (index < 0) return MEISO_INGRESS_VIDEO_WOULD_BLOCK;
    if (sync_for_cpu_write(v->input[index].fd, 1)) { v->state = FAILED; return MEISO_INGRESS_VIDEO_SUBMIT_ERROR; }
    memcpy(v->input[index].map, au, au_size);
    if (sync_for_cpu_write(v->input[index].fd, 0)) { v->state = FAILED; return MEISO_INGRESS_VIDEO_SUBMIT_ERROR; }
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_DMABUF,
        .index = (unsigned)index, .bytesused = au_size };
    b.m.fd = v->input[index].fd; b.length = v->input_v4l_length[index];
    /* MVX maps V4L2_BUF_FLAG_KEYFRAME on its decode bitstream port to EOF.
     * Each accepted input is one complete H.264 access unit. */
    b.flags = V4L2_BUF_FLAG_TIMESTAMP_COPY | V4L2_BUF_FLAG_KEYFRAME;
    b.timestamp.tv_sec = (long)(sequence / UINT64_C(1000000)); b.timestamp.tv_usec = (long)(sequence % UINT64_C(1000000));
    if (ioctl(v->video_fd, VIDIOC_QBUF, &b)) { v->state = FAILED; return MEISO_INGRESS_VIDEO_SUBMIT_ERROR; }
    v->input_busy[index] = 1; return MEISO_INGRESS_VIDEO_ACCEPTED;
}

MeisoIngressVideoPollResult meiso_ingress_video_poll(MeisoIngressVideo *v, MeisoIngressVideoPresentation *out)
{
    if (!v || !out || v->state == FAILED) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    if (reap_output(v) || service_source_change(v)) goto fail;
    if (v->flip.pending || v->overlay.pending) {
        if (v->overlay.pending && v->overlay.seen) return MEISO_INGRESS_VIDEO_NONE;
        struct pollfd p = { .fd = v->drm_fd, .events = POLLIN };
        int n = poll(&p, 1, 33);
        if (n < 0 && errno != EINTR) goto fail;
        if (n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) goto fail;
        if (n > 0 && (p.revents & POLLIN)) {
            drmEventContext e = { .version = 3, .page_flip_handler2 = page_flip };
            if (drmHandleEvent(v->drm_fd, &e)) goto fail;
        }
        if (v->overlay.pending) return MEISO_INGRESS_VIDEO_NONE;
        int done = finish_flip(v, out); if (done < 0) goto fail;
        return done ? MEISO_INGRESS_VIDEO_PRESENTED : MEISO_INGRESS_VIDEO_NONE;
    }
    if (v->state != READY) return MEISO_INGRESS_VIDEO_NONE;
    struct v4l2_plane p[2] = {0};
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_DMABUF, .length = 2 };
    b.m.planes = p;
    if (ioctl(v->video_fd, VIDIOC_DQBUF, &b)) return errno == EAGAIN ? MEISO_INGRESS_VIDEO_NONE : MEISO_INGRESS_VIDEO_POLL_ERROR;
    if (b.index >= (uint32_t)v->capture_count) goto fail;
    if (!(b.flags & V4L2_BUF_FLAG_MVX_BUFFER_FRAME_PRESENT)) {
        if (queue_capture(v, b.index)) goto fail;
        return MEISO_INGRESS_VIDEO_NONE;
    }
    if (start_present(v, b.index, &b)) goto fail;
    return meiso_ingress_video_poll(v, out);
fail:
    v->state = FAILED; return MEISO_INGRESS_VIDEO_POLL_ERROR;
}

int meiso_ingress_video_output_size(const MeisoIngressVideo *v, uint32_t *width, uint32_t *height)
{
    if (!v || !width || !height || v->state == FAILED) return -1;
    *width=v->output_w; *height=v->output_h; return 0;
}

static int setup_overlay(struct MeisoIngressVideo *v)
{
    if (v->overlay.plane) return 0;
    drmModePlaneRes *planes = drmModeGetPlaneResources(v->drm_fd);
    if (!planes) return -1;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < planes->count_planes && !chosen; ++i) {
        drmModePlane *p = drmModeGetPlane(v->drm_fd, planes->planes[i]);
        if (!p) continue;
        int argb = 0, is_overlay = 0;
        for (uint32_t j = 0; j < p->count_formats; ++j)
            if (p->formats[j] == DRM_FORMAT_ARGB8888) argb = 1;
        drmModeObjectProperties *props = drmModeObjectGetProperties(
            v->drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props) for (uint32_t j = 0; j < props->count_props; ++j) {
            drmModePropertyRes *prop = drmModeGetProperty(v->drm_fd, props->props[j]);
            if (prop && !strcmp(prop->name, "type") && props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY)
                is_overlay = 1;
            if (prop) drmModeFreeProperty(prop);
        }
        if (props) drmModeFreeObjectProperties(props);
        if (argb && is_overlay && !p->fb_id && !p->crtc_id && (p->possible_crtcs & 1U))
            chosen = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(planes);
    if (!chosen || read_properties_at(v->drm_fd, chosen, v->overlay.props, v->overlay.before)) return -1;
    v->overlay.plane = chosen;
    return 0;
}

MeisoIngressVideoSubmitResult meiso_ingress_video_overlay_submit(
    MeisoIngressVideo *v, uint64_t sequence, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height, const uint32_t *pixels, size_t bytes)
{
    if (!v || v->state == FAILED || !sequence || sequence <= v->overlay.last_sequence)
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if (v->flip.pending || v->overlay.pending) return MEISO_INGRESS_VIDEO_WOULD_BLOCK;
    const int disable = width == 0 && height == 0 && pixels == NULL && bytes == 0;
    const uint64_t needed = (uint64_t)width * height * 4;
    if ((!disable && (!pixels || !width || !height || needed > OVERLAY_BYTES || bytes != needed ||
            width > v->output_w || height > v->output_h ||
            x > v->output_w - width || y > v->output_h - height)) ||
        (disable && (v->overlay.front < 0 || x || y))) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    if (setup_overlay(v)) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    uint64_t values[NPROPS];
    int target = -1;
    if (disable) memcpy(values, v->overlay.before, sizeof(values));
    else {
        target = v->overlay.front == 0 ? 1 : 0;
        for (int i = 0; i < 2; ++i)
            if (v->overlay.surfaces[i].fd < 0 &&
                make_surface(v->drm_fd, OVERLAY_BYTES, 1, &v->overlay.surfaces[i]))
                return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
        struct surface *surface = &v->overlay.surfaces[target];
        if (sync_for_cpu_write(surface->fd, 1)) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
        memcpy(surface->map, pixels, bytes);
        if (sync_for_cpu_write(surface->fd, 0)) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
        if (v->overlay.fb[target] &&
            (v->overlay.width[target] != width || v->overlay.height[target] != height)) {
            if (drmModeRmFB(v->drm_fd, v->overlay.fb[target])) return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
            v->overlay.fb[target] = 0;
        }
        if (!v->overlay.fb[target]) {
            uint32_t handles[4] = {surface->handle}, pitches[4] = {width * 4}, offsets[4] = {0};
            if (drmModeAddFB2(v->drm_fd, width, height, DRM_FORMAT_ARGB8888,
                    handles, pitches, offsets, &v->overlay.fb[target], 0))
                return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
            v->overlay.width[target] = width; v->overlay.height[target] = height;
        }
        const uint64_t candidate[NPROPS] = {v->overlay.fb[target], v->crtc, 0, 0,
            (uint64_t)width << 16, (uint64_t)height << 16, x, y, width, height};
        memcpy(values, candidate, sizeof(values));
    }
    if (atomic_plane_commit(v->drm_fd, v->overlay.plane, v->overlay.props, values,
                            DRM_MODE_ATOMIC_TEST_ONLY, NULL) ||
        atomic_plane_commit(v->drm_fd, v->overlay.plane, v->overlay.props, values,
                            DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, v))
        return MEISO_INGRESS_VIDEO_SUBMIT_ERROR;
    v->overlay.target = target; v->overlay.sequence = sequence; v->overlay.last_sequence = sequence;
    v->overlay.pending = 1; v->overlay.seen = 0;
    return MEISO_INGRESS_VIDEO_ACCEPTED;
}

MeisoIngressVideoPollResult meiso_ingress_video_overlay_poll(
    MeisoIngressVideo *v, MeisoIngressVideoPresentation *out)
{
    if (!v || !out || v->state == FAILED) return MEISO_INGRESS_VIDEO_POLL_ERROR;
    if (!v->overlay.pending) return MEISO_INGRESS_VIDEO_NONE;
    if (!v->overlay.seen) {
        struct pollfd p = { .fd = v->drm_fd, .events = POLLIN };
        const int n = poll(&p, 1, 0);
        if ((n < 0 && errno != EINTR) || (n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))))
            goto fail;
        if (n > 0 && (p.revents & POLLIN)) {
            drmEventContext e = { .version = 3, .page_flip_handler2 = page_flip };
            if (drmHandleEvent(v->drm_fd, &e)) goto fail;
        }
    }
    if (!v->overlay.seen) return MEISO_INGRESS_VIDEO_NONE;
    *out = (MeisoIngressVideoPresentation){v->overlay.sequence, v->overlay.ns,
        v->output_w, v->output_h, v->connector};
    v->overlay.front = v->overlay.target; v->overlay.pending = 0; v->overlay.seen = 0;
    return MEISO_INGRESS_VIDEO_PRESENTED;
fail:
    /* DRM is shared with video. Preserve unknown scanout ownership until
     * close restores it; continuing video here would stall on pending HUD. */
    v->state = FAILED; return MEISO_INGRESS_VIDEO_POLL_ERROR;
}

int meiso_ingress_video_close(MeisoIngressVideo *v)
{
    int rc = 0;
    if (!v) { errno = EINVAL; return -1; }
    if (v->video_fd >= 0) {
        enum v4l2_buf_type cap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, out = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        if (v->capture_streaming && stream(v, cap, 0)) rc = -1;
        v->capture_streaming = 0;
        if (v->output_streaming && stream(v, out, 0)) rc = -1;
        v->output_streaming = 0;
        if (v->capture_buffers_requested && request_buffers(v, cap, 0)) rc = -1;
        if (v->output_buffers_requested && request_buffers(v, out, 0)) rc = -1;
        if (v->source_subscribed) {
            struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
            if (ioctl(v->video_fd, VIDIOC_UNSUBSCRIBE_EVENT, &sub)) rc = -1;
            v->source_subscribed = 0;
        }
    }
    /* No decoder queue retains the DMA-BUFs before replacing the scanout plane. */
    if (v->drm_fd >= 0 && v->overlay.plane &&
        atomic_plane_commit(v->drm_fd, v->overlay.plane, v->overlay.props,
                            v->overlay.before, 0, NULL)) rc = -1;
    if (v->drm_fd >= 0 && v->plane && atomic_commit(v, v->before, v->right.before, 0, NULL)) rc = -1;
    if (v->drm_fd >= 0) {
        if (release_fb(v, v->shown_fb, v->shown_handles)) rc = -1;
        if (release_fb(v, v->flip.fb, v->flip.handles)) rc = -1;
    }
    if (v->video_fd >= 0 && close(v->video_fd)) rc = -1;
    for (int i = 0; i < CAPTURE_BUFFERS; ++i) {
        if (destroy_surface(v->drm_fd, &v->capture[i][0])) rc = -1;
        if (destroy_surface(v->drm_fd, &v->capture[i][1])) rc = -1;
    }
    for (int i = 0; i < INPUT_BUFFERS; ++i) if (destroy_surface(v->drm_fd, &v->input[i])) rc = -1;
    for (int i = 0; i < 2; ++i) {
        if (v->overlay.fb[i] && drmModeRmFB(v->drm_fd, v->overlay.fb[i])) rc = -1;
        if (destroy_surface(v->drm_fd, &v->overlay.surfaces[i])) rc = -1;
    }
    if (destroy_surface(v->drm_fd, &v->dummy[0])) rc = -1;
    if (destroy_surface(v->drm_fd, &v->dummy[1])) rc = -1;
    if (v->drm_fd >= 0 && close(v->drm_fd)) rc = -1;
    free(v);
    return rc;
}
