#include <meiso/profiles/display_ingress/planes.h>
#include <meiso/profiles/display_ingress/camera.h>
#include "display_ingress_profile_private.h"
#include <math.h>
#include <string.h>
#define put32 meiso_display_ingress_private_put_u32
#define put64 meiso_display_ingress_private_put_u64
#define get32 meiso_display_ingress_private_get_u32
#define get64 meiso_display_ingress_private_get_u64
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap

/* "DISPLAY_" / "DISP_SCH" prefixes, as in the other Display Ingress resources. */
#define DISPLAY_ UINT64_C(0x444953504c41595f)
#define DISP_SCH UINT64_C(0x444953505f534348)
static const MeisoEndpointId endpoint = {DISPLAY_, UINT64_C(0x504c414e5f454e44)};            /* PLAN_END */
static const MeisoSchemaRef capability_schema = {{DISP_SCH, UINT64_C(0x504c4e5f4341505f)}, 1}; /* PLN_CAP_ */
static const uint64_t resource_suffix[4] = {
    UINT64_C(0x504c4e5f4445535f), /* PLN_DES_ */
    UINT64_C(0x504c4e5f4143545f), /* PLN_ACT_ */
    UINT64_C(0x504c4e5f50524553), /* PLN_PRES */
    UINT64_C(0x504c4e5f494d475f)  /* PLN_IMG_ */
};
static const uint64_t schema_suffix[4] = {
    UINT64_C(0x504c4e5f4d4f444c), /* PLN_MODL */
    UINT64_C(0x504c4e5f53544154), /* PLN_STAT */
    UINT64_C(0x504c4e5f50524553), /* PLN_PRES */
    UINT64_C(0x504c4e5f4a504547)  /* PLN_JPEG */
};

const MeisoEndpointId *meiso_display_planes_endpoint(void) { return &endpoint; }

/* Tracking endpoint (planes-v12.md §10). */
static const MeisoSchemaRef tracking_capability_schema = {{DISP_SCH, UINT64_C(0x54524b5f4341505f)}, 1}; /* TRK_CAP_ */
static const uint64_t tracking_resource_suffix[4] = {
    UINT64_C(0x54524b5f4445535f), /* TRK_DES_ */
    UINT64_C(0x54524b5f4143545f), /* TRK_ACT_ */
    UINT64_C(0x54524b5f494d555f), /* TRK_IMU_ */
    UINT64_C(0x54524b5f504f5345)  /* TRK_POSE */
};
static const uint64_t tracking_schema_suffix[4] = {
    UINT64_C(0x54524b5f44455349), /* TRK_DESI */
    UINT64_C(0x54524b5f53544154), /* TRK_STAT */
    UINT64_C(0x54524b5f494d555f), /* TRK_IMU_ */
    UINT64_C(0x54524b5f504f5345)  /* TRK_POSE */
};

size_t meiso_display_planes_tracking_route_base(const MeisoDisplayPlanesCatalogConfig *c)
{
    return c ? 8U + 3U * c->camera_enabled : 0;
}

size_t meiso_display_planes_local_pose_route_base(const MeisoDisplayPlanesCatalogConfig *c)
{ return meiso_display_planes_tracking_route_base(c); }

static const MeisoSchemaRef local_pose_capability_schema = {{DISP_SCH, UINT64_C(0x4c50535f4341505f)}, 1};
static const MeisoSchemaRef views_capability_schema = {{DISP_SCH, UINT64_C(0x5657535f4341505f)}, 1};
static const uint64_t local_pose_resource_suffix[3] = {
    UINT64_C(0x4c50535f4445535f), UINT64_C(0x4c50535f4143545f), UINT64_C(0x4c50535f504f5345)
};
static const uint64_t local_pose_schema_suffix[3] = {
    UINT64_C(0x4c50535f44455349), UINT64_C(0x4c50535f53544154), UINT64_C(0x4c50535f504f5345)
};

/* Input endpoint (planes-v12.md §11). */
static const MeisoSchemaRef input_capability_schema = {{DISP_SCH, UINT64_C(0x494e505f4341505f)}, 1}; /* INP_CAP_ */
static const uint64_t input_resource_suffix[3] = {
    UINT64_C(0x494e505f4445535f), /* INP_DES_ */
    UINT64_C(0x494e505f4143545f), /* INP_ACT_ */
    UINT64_C(0x494e505f4b45595f)  /* INP_KEY_ */
};
static const uint64_t input_schema_suffix[3] = {
    UINT64_C(0x494e505f44455349), /* INP_DESI */
    UINT64_C(0x494e505f53544154), /* INP_STAT */
    UINT64_C(0x494e505f4b45595f)  /* INP_KEY_ */
};

size_t meiso_display_planes_input_route_base(const MeisoDisplayPlanesCatalogConfig *c)
{
    return c ? 8U + 3U * c->camera_enabled + 4U * c->tracking_enabled + 3U * c->local_pose_enabled : 0;
}

/* Audio endpoint (planes-v12.md §15); views has no routes, so audio's follow input's. */
static const MeisoSchemaRef audio_capability_schema = {{DISP_SCH, UINT64_C(0x4155445f4341505f)}, 1}; /* AUD_CAP_ */
static const uint64_t audio_resource_suffix[3] = {
    UINT64_C(0x4155445f4445535f), /* AUD_DES_ */
    UINT64_C(0x4155445f4143545f), /* AUD_ACT_ */
    UINT64_C(0x4155445f50434d5f)  /* AUD_PCM_ */
};
static const uint64_t audio_schema_suffix[3] = {
    UINT64_C(0x4155445f44455349), /* AUD_DESI */
    UINT64_C(0x4155445f53544154), /* AUD_STAT */
    UINT64_C(0x4155445f50434d5f)  /* AUD_PCM_ */
};

size_t meiso_display_planes_audio_route_base(const MeisoDisplayPlanesCatalogConfig *c)
{
    return c ? meiso_display_planes_input_route_base(c) + 3U * c->input_enabled : 0;
}

static void putf(uint8_t *b, float v) { uint32_t u; memcpy(&u, &v, 4); put32(b, u); }
static float getf(const uint8_t *b) { uint32_t u = get32(b); float v; memcpy(&v, &u, 4); return v; }
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] * 256U + b[1]); }
static bool finite_in(float v, float lo, float hi) { return isfinite(v) && v >= lo && v <= hi; }
static bool dimension(uint32_t n) { return n && n <= 4096; }
static bool positive_zero(float v) { return v == 0.0f && !signbit(v); }

static bool cap_valid(const MeisoDisplayPlanesCapability *c)
{
    const float half_pi = 1.5707963f;
    if (!c || c->view_layout < MEISO_DISPLAY_PLANES_VIEW_MONO || c->view_layout > MEISO_DISPLAY_PLANES_VIEW_RUNTIME_VIEWS ||
        c->tracking > MEISO_DISPLAY_PLANES_TRACKING_EDGE ||
        !c->max_planes || c->max_planes > MEISO_DISPLAY_PLANES_MAX_PLANES ||
        !c->max_images || c->max_images > MEISO_DISPLAY_PLANES_MAX_IMAGES ||
        !c->max_image_bytes || c->max_image_bytes > MEISO_DISPLAY_PLANES_MAX_IMAGE_BYTES ||
        !dimension(c->max_image_width) || !dimension(c->max_image_height) ||
        !c->max_updates_per_second || c->max_updates_per_second > 60) return false;
    if ((c->motion & ~(MEISO_DISPLAY_PLANES_MOTION_TRANSITION | MEISO_DISPLAY_PLANES_MOTION_FOLLOW)) ||
        ((c->motion & MEISO_DISPLAY_PLANES_MOTION_FOLLOW) && !(c->motion & MEISO_DISPLAY_PLANES_MOTION_TRANSITION)))
        return false;
    if (c->view_layout == MEISO_DISPLAY_PLANES_VIEW_RUNTIME_VIEWS)
        return !c->output_width && !c->output_height && positive_zero(c->fov_left) && positive_zero(c->fov_right) &&
            positive_zero(c->fov_up) && positive_zero(c->fov_down) && positive_zero(c->ipd_m);
    if (!dimension(c->output_width) || !dimension(c->output_height)) return false;
    if (!isfinite(c->fov_left) || !isfinite(c->fov_right) || !isfinite(c->fov_up) || !isfinite(c->fov_down) ||
        !(c->fov_left > -half_pi && c->fov_left < c->fov_right && c->fov_right < half_pi) ||
        !(c->fov_down > -half_pi && c->fov_down < c->fov_up && c->fov_up < half_pi)) return false;
    if (c->view_layout == MEISO_DISPLAY_PLANES_VIEW_MONO) return c->ipd_m == 0.0f && !signbit(c->ipd_m);
    return finite_in(c->ipd_m, 0.04f, 0.08f) && !(c->output_width % 2);
}

bool meiso_display_planes_capability_encode(const MeisoDisplayPlanesCapability *c, MeisoMutableBuffer out)
{
    if (!cap_valid(c) || !out.data || out.capacity < 64 || overlap(c, sizeof(*c), out.data, 64)) return false;
    uint8_t b[64] = {0};
    put32(b, c->output_width); put32(b + 4, c->output_height);
    b[8] = c->view_layout; b[9] = c->tracking; b[10] = c->max_planes; b[11] = c->max_images;
    put32(b + 12, c->max_image_bytes); put16(b + 16, c->max_image_width); put16(b + 18, c->max_image_height);
    putf(b + 20, c->fov_left); putf(b + 24, c->fov_right); putf(b + 28, c->fov_up); putf(b + 32, c->fov_down);
    putf(b + 36, c->ipd_m); put16(b + 40, c->max_updates_per_second); b[42] = c->motion;
    memcpy(out.data, b, 64); return true;
}

bool meiso_display_planes_capability_decode(MeisoByteView in, MeisoDisplayPlanesCapability *out)
{
    if (!in.data || in.size != 64 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 43, 21))
        return false;
    const uint8_t *b = in.data;
    MeisoDisplayPlanesCapability c = {0};
    c.output_width = get32(b); c.output_height = get32(b + 4);
    c.view_layout = b[8]; c.tracking = b[9]; c.max_planes = b[10]; c.max_images = b[11];
    c.max_image_bytes = get32(b + 12); c.max_image_width = get16(b + 16); c.max_image_height = get16(b + 18);
    c.fov_left = getf(b + 20); c.fov_right = getf(b + 24); c.fov_up = getf(b + 28); c.fov_down = getf(b + 32);
    c.ipd_m = getf(b + 36); c.max_updates_per_second = get16(b + 40); c.motion = b[42];
    if (!cap_valid(&c)) return false;
    *out = c; return true;
}

/* Strict UTF-8 (no overlongs, surrogates or code points above U+10FFFF), no C0/C1 controls or DEL. */
static bool name_valid(const char *s, uint8_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    if (!n || n > MEISO_DISPLAY_PLANES_MAX_NAME || !zero(p + n, MEISO_DISPLAY_PLANES_MAX_NAME - n)) return false;
    for (size_t i = 0; i < n;) {
        uint32_t cp = p[i], len = cp < 0x80 ? 1 : (cp & 0xe0) == 0xc0 ? 2 : (cp & 0xf0) == 0xe0 ? 3 : (cp & 0xf8) == 0xf0 ? 4 : 0;
        if (!len || i + len > n) return false;
        if (len > 1) cp &= 0xffU >> (len + 1);
        for (uint32_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3f);
        }
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return false;
        i += len;
    }
    return true;
}

static bool plane_valid(const MeisoDisplayPlane *v, uint8_t app_count)
{
    const float *q = v->orientation;
    float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!v->plane_id || (v->app != MEISO_DISPLAY_PLANES_NO_APP && v->app >= app_count) ||
        v->anchor < MEISO_DISPLAY_PLANES_ANCHOR_HEAD || v->anchor > MEISO_DISPLAY_PLANES_ANCHOR_FOLLOW ||
        v->transition_cs > MEISO_DISPLAY_PLANES_MAX_TRANSITION_CS ||
        (v->motion_style & 0x0f) > MEISO_DISPLAY_PLANES_CURVE_EASE_IN_OUT ||
        (v->motion_style >> 4) > MEISO_DISPLAY_PLANES_ENTER_FADE_SCALE ||
        (v->anchor == MEISO_DISPLAY_PLANES_ANCHOR_FOLLOW ?
            !v->follow_deg || v->follow_deg > MEISO_DISPLAY_PLANES_MAX_FOLLOW_DEG : v->follow_deg != 0) ||
        (v->source != MEISO_DISPLAY_PLANES_SOURCE_STREAM && v->source != MEISO_DISPLAY_PLANES_SOURCE_IMAGE) ||
        !dimension(v->source_width) || !dimension(v->source_height) ||
        v->source_x > 4096 - v->source_width || v->source_y > 4096 - v->source_height) return false;
    if (v->source == MEISO_DISPLAY_PLANES_SOURCE_STREAM ? !zero(v->image_sha256, 32) : zero(v->image_sha256, 32))
        return false;
    for (int i = 0; i < 3; ++i) if (!finite_in(v->position[i], -1000.0f, 1000.0f)) return false;
    for (int i = 0; i < 4; ++i) if (!finite_in(q[i], -1.001f, 1.001f)) return false;
    return n >= 0.999f && n <= 1.001f && isfinite(v->width_m) && isfinite(v->height_m) &&
        v->width_m > 0.0f && v->width_m <= 100.0f && v->height_m > 0.0f && v->height_m <= 100.0f;
}

static bool model_valid(const MeisoDisplayPlanesModel *m)
{
    if (!m || !m->generation || m->plane_count > MEISO_DISPLAY_PLANES_MAX_PLANES ||
        m->app_count > MEISO_DISPLAY_PLANES_MAX_APPS ||
        (m->focused_app != MEISO_DISPLAY_PLANES_NO_APP && m->focused_app >= m->app_count)) return false;
    bool stream = false;
    for (uint8_t i = 0; i < m->app_count; ++i) {
        if (!m->apps[i].app_id || !name_valid(m->apps[i].name, m->apps[i].name_size)) return false;
        for (uint8_t j = 0; j < i; ++j) if (m->apps[j].app_id == m->apps[i].app_id) return false;
    }
    for (uint8_t i = 0; i < m->plane_count; ++i) {
        if (!plane_valid(&m->planes[i], m->app_count)) return false;
        for (uint8_t j = 0; j < i; ++j) if (m->planes[j].plane_id == m->planes[i].plane_id) return false;
        stream |= m->planes[i].source == MEISO_DISPLAY_PLANES_SOURCE_STREAM;
    }
    return stream == (m->video_desired_version != 0);
}

size_t meiso_display_planes_model_bytes(const MeisoDisplayPlanesModel *m)
{
    return model_valid(m) ? (size_t)32 + 40U * m->app_count + 96U * m->plane_count : 0;
}

bool meiso_display_planes_model_encode(const MeisoDisplayPlanesModel *m, MeisoMutableBuffer out)
{
    const size_t n = meiso_display_planes_model_bytes(m);
    if (!n || !out.data || out.capacity < n || overlap(m, sizeof(*m), out.data, n)) return false;
    uint8_t b[MEISO_DISPLAY_PLANES_MAX_MODEL_BYTES] = {0};
    put64(b, m->generation); put64(b + 8, m->video_desired_version);
    b[16] = m->plane_count; b[17] = m->app_count; b[18] = m->focused_app;
    put32(b + 24, m->world_epoch);
    uint8_t *s = b + 32;
    for (uint8_t i = 0; i < m->app_count; ++i, s += 40) {
        put32(s, m->apps[i].app_id); s[4] = m->apps[i].name_size; memcpy(s + 8, m->apps[i].name, m->apps[i].name_size);
    }
    for (uint8_t i = 0; i < m->plane_count; ++i, s += 96) {
        const MeisoDisplayPlane *v = &m->planes[i];
        put32(s, v->plane_id); s[4] = v->app; s[5] = v->anchor; s[6] = v->source; s[7] = v->opacity;
        put32(s + 8, v->source_x); put32(s + 12, v->source_y); put32(s + 16, v->source_width); put32(s + 20, v->source_height);
        memcpy(s + 24, v->image_sha256, 32);
        for (int k = 0; k < 3; ++k) putf(s + 56 + 4 * k, v->position[k]);
        for (int k = 0; k < 4; ++k) putf(s + 68 + 4 * k, v->orientation[k]);
        putf(s + 84, v->width_m); putf(s + 88, v->height_m);
        s[92] = v->transition_cs; s[93] = v->motion_style; s[94] = v->follow_deg;
    }
    memcpy(out.data, b, n); return true;
}

bool meiso_display_planes_model_decode(MeisoByteView in, MeisoDisplayPlanesModel *out)
{
    if (!in.data || in.size < 32 || !out || overlap(in.data, in.size, out, sizeof(*out))) return false;
    const uint8_t *b = in.data;
    if (b[16] > MEISO_DISPLAY_PLANES_MAX_PLANES || b[17] > MEISO_DISPLAY_PLANES_MAX_APPS ||
        in.size != (size_t)32 + 40U * b[17] + 96U * b[16] || !zero(b + 19, 5) || !zero(b + 28, 4)) return false;
    MeisoDisplayPlanesModel m;
    memset(&m, 0, sizeof(m));
    m.generation = get64(b); m.video_desired_version = get64(b + 8);
    m.plane_count = b[16]; m.app_count = b[17]; m.focused_app = b[18];
    m.world_epoch = get32(b + 24);
    const uint8_t *s = b + 32;
    for (uint8_t i = 0; i < m.app_count; ++i, s += 40) {
        if (!zero(s + 5, 3)) return false;
        m.apps[i].app_id = get32(s); m.apps[i].name_size = s[4]; memcpy(m.apps[i].name, s + 8, 32);
    }
    for (uint8_t i = 0; i < m.plane_count; ++i, s += 96) {
        MeisoDisplayPlane *v = &m.planes[i];
        if (s[95]) return false;
        v->plane_id = get32(s); v->app = s[4]; v->anchor = s[5]; v->source = s[6]; v->opacity = s[7];
        v->source_x = get32(s + 8); v->source_y = get32(s + 12); v->source_width = get32(s + 16); v->source_height = get32(s + 20);
        memcpy(v->image_sha256, s + 24, 32);
        for (int k = 0; k < 3; ++k) v->position[k] = getf(s + 56 + 4 * k);
        for (int k = 0; k < 4; ++k) v->orientation[k] = getf(s + 68 + 4 * k);
        v->width_m = getf(s + 84); v->height_m = getf(s + 88);
        v->transition_cs = s[92]; v->motion_style = s[93]; v->follow_deg = s[94];
    }
    if (!model_valid(&m)) return false;
    *out = m; return true;
}

uint32_t meiso_display_planes_model_image_count(const MeisoDisplayPlanesModel *m)
{
    if (!model_valid(m)) return 0;
    uint32_t count = 0;
    for (uint8_t i = 0; i < m->plane_count; ++i) {
        const MeisoDisplayPlane *v = &m->planes[i];
        bool seen = false;
        if (v->source != MEISO_DISPLAY_PLANES_SOURCE_IMAGE) continue;
        for (uint8_t j = 0; j < i && !seen; ++j)
            seen = m->planes[j].source == MEISO_DISPLAY_PLANES_SOURCE_IMAGE && !memcmp(m->planes[j].image_sha256, v->image_sha256, 32);
        count += !seen;
    }
    return count;
}

bool meiso_display_planes_model_supported(const MeisoDisplayPlanesCapability *c, const MeisoDisplayPlanesModel *m,
    uint64_t video_version, uint32_t video_width, uint32_t video_height)
{
    if (!cap_valid(c) || !model_valid(m) || m->plane_count > c->max_planes ||
        ((c->tracking == MEISO_DISPLAY_PLANES_TRACKING_EDGE) != (m->world_epoch != 0)) ||
        meiso_display_planes_model_image_count(m) > c->max_images) return false;
    if (m->video_desired_version && (m->video_desired_version != video_version || !video_width || !video_height)) return false;
    for (uint8_t i = 0; i < m->plane_count; ++i) {
        const MeisoDisplayPlane *v = &m->planes[i];
        /* §16: motion fields and the FOLLOW anchor only for an Edge that declares them. */
        if (!(c->motion & MEISO_DISPLAY_PLANES_MOTION_TRANSITION) && (v->transition_cs || v->motion_style || v->follow_deg))
            return false;
        if (v->anchor == MEISO_DISPLAY_PLANES_ANCHOR_FOLLOW && !(c->motion & MEISO_DISPLAY_PLANES_MOTION_FOLLOW)) return false;
        if (v->source == MEISO_DISPLAY_PLANES_SOURCE_STREAM &&
            (v->source_width > video_width || v->source_x > video_width - v->source_width ||
             v->source_height > video_height || v->source_y > video_height - v->source_height)) return false;
        if (v->source == MEISO_DISPLAY_PLANES_SOURCE_IMAGE &&
            (v->source_x + v->source_width > c->max_image_width || v->source_y + v->source_height > c->max_image_height))
            return false;
    }
    return true;
}

bool meiso_display_planes_status_encode(const MeisoDisplayPlanesStatus *v, MeisoMutableBuffer out)
{
    if (!v || !out.data || out.capacity < 16 || overlap(v, sizeof(*v), out.data, 16) ||
        v->drawn > MEISO_DISPLAY_PLANES_MAX_PLANES || v->waiting > MEISO_DISPLAY_PLANES_MAX_PLANES - v->drawn ||
        (!v->generation && (v->drawn || v->waiting))) return false;
    uint8_t b[16] = {0};
    put64(b, v->generation); b[8] = v->drawn; b[9] = v->waiting;
    memcpy(out.data, b, 16); return true;
}

bool meiso_display_planes_status_decode(MeisoByteView in, MeisoDisplayPlanesStatus *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 10, 6))
        return false;
    MeisoDisplayPlanesStatus v = {get64(in.data), in.data[8], in.data[9]};
    uint8_t check[16];
    if (!meiso_display_planes_status_encode(&v, (MeisoMutableBuffer){check, 16})) return false;
    *out = v; return true;
}

bool meiso_display_planes_evidence_encode(const MeisoDisplayPlanesEvidence *v, MeisoMutableBuffer out)
{
    if (!v || !out.data || out.capacity < 64 || overlap(v, sizeof(*v), out.data, 64) ||
        (v->evidence_class != MEISO_DISPLAY_INGRESS_EVIDENCE_SIMULATED &&
         v->evidence_class != MEISO_DISPLAY_INGRESS_EVIDENCE_DRM_KMS_PAGE_FLIP &&
         v->evidence_class != MEISO_DISPLAY_INGRESS_EVIDENCE_XR_FRAME_SUBMITTED) ||
        ((v->evidence_class == MEISO_DISPLAY_INGRESS_EVIDENCE_XR_FRAME_SUBMITTED) != (v->predicted_display_time_ns != 0)) ||
        !v->generation || !v->desired_version || v->drawn > MEISO_DISPLAY_PLANES_MAX_PLANES ||
        v->waiting > MEISO_DISPLAY_PLANES_MAX_PLANES - v->drawn) return false;
    uint8_t b[64] = {0};
    b[0] = v->evidence_class; b[1] = v->drawn; b[2] = v->waiting;
    put64(b + 8, v->generation); put64(b + 16, v->desired_version); memcpy(b + 24, v->model_sha256, 32);
    put64(b + 56, v->predicted_display_time_ns);
    memcpy(out.data, b, 64); return true;
}

bool meiso_display_planes_evidence_decode(MeisoByteView in, MeisoDisplayPlanesEvidence *out)
{
    if (!in.data || in.size != 64 || !out || overlap(in.data, in.size, out, sizeof(*out)) ||
        !zero(in.data + 3, 5)) return false;
    MeisoDisplayPlanesEvidence v;
    memset(&v, 0, sizeof(v));
    v.evidence_class = in.data[0]; v.drawn = in.data[1]; v.waiting = in.data[2];
    v.generation = get64(in.data + 8); v.desired_version = get64(in.data + 16); memcpy(v.model_sha256, in.data + 24, 32);
    v.predicted_display_time_ns = get64(in.data + 56);
    uint8_t check[64];
    if (!meiso_display_planes_evidence_encode(&v, (MeisoMutableBuffer){check, 64})) return false;
    *out = v; return true;
}

bool meiso_display_planes_sha256(MeisoByteView bytes, uint8_t out[32])
{
    return out && (bytes.data || !bytes.size) && !overlap(bytes.data, bytes.size, out, 32) &&
        meiso_display_ingress_private_payload_sha256(bytes, out);
}

bool meiso_display_planes_jpeg_header(MeisoByteView in, uint32_t *width, uint32_t *height)
{
    if (!in.data || in.size < 4 || !width || !height || in.data[0] != 0xff || in.data[1] != 0xd8) return false;
    size_t i = 2;
    while (i + 4 <= in.size) {
        if (in.data[i] != 0xff) return false;
        uint8_t marker = in.data[i + 1];
        if (marker == 0xff) { ++i; continue; }
        size_t length = (size_t)in.data[i + 2] * 256U + in.data[i + 3];
        if (length < 2 || i + 2 + length > in.size) return false;
        if (marker == 0xc0) {
            const uint8_t *f = in.data + i + 4;
            if (length < 8) return false;
            uint32_t h = (uint32_t)f[1] * 256U + f[2], w = (uint32_t)f[3] * 256U + f[4];
            if (f[0] != 8 || !w || !h || (f[5] != 1 && f[5] != 3) || length != 8U + 3U * f[5]) return false;
            *width = w; *height = h; return true;
        }
        /* Any other frame type (progressive, lossless, arithmetic...) is outside the baseline contract. */
        if ((marker >= 0xc1 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) ||
            marker == 0xda || marker == 0xd9) return false;
        i += 2 + length;
    }
    return false;
}

/* ---- Session, Runtime and Catalog (planes-v12.md §2) ---- */

bool meiso_display_planes_configure_session(MeisoSessionConfig *out)
{
    if (!out) return false;
    MeisoSessionConfig c = *out;
    if (meiso_display_ingress_configure_session_for_format(&c, MEISO_DISPLAY_INGRESS_FRAME_H264_IDR) !=
        MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    c.profile.version = MEISO_DISPLAY_PLANES_PROFILE_VERSION;
    c.limits.max_registry_entries = MEISO_DISPLAY_PLANES_MAX_ENDPOINTS + MEISO_DISPLAY_PLANES_MAX_ROUTES;
    if (!meiso_session_memory_required(&c)) return false;
    *out = c; return true;
}

static uint32_t negotiated_registry(const MeisoSession *session)
{
    MeisoSessionSnapshot s;
    return session && meiso_session_snapshot(session, &s) && s.state == MEISO_SESSION_STATE_READY ?
        s.negotiated_limits.max_registry_entries : 0;
}

bool meiso_display_planes_session_allows_tracking(const MeisoSession *session)
{
    return negotiated_registry(session) >= MEISO_DISPLAY_PLANES_TRACKING_REGISTRY;
}

bool meiso_display_planes_session_allows_input(const MeisoSession *session)
{
    return negotiated_registry(session) >= MEISO_DISPLAY_PLANES_INPUT_REGISTRY;
}

bool meiso_display_planes_evidence_supported(const MeisoDisplayIngressCapability *c, const MeisoDisplayPlanesEvidence *e)
{
    uint8_t check[MEISO_DISPLAY_PLANES_EVIDENCE_BYTES];
    return meiso_display_ingress_private_capability_valid(c) &&
        meiso_display_planes_evidence_encode(e, (MeisoMutableBuffer){check, sizeof(check)}) &&
        (c->evidence_classes & (UINT32_C(1) << (e->evidence_class - 1U))) != 0U;
}

bool meiso_display_planes_session_allows_edge_extensions(const MeisoSession *session)
{
    return negotiated_registry(session) >= MEISO_DISPLAY_PLANES_EDGE_REGISTRY;
}

bool meiso_display_planes_session_allows_audio(const MeisoSession *session)
{
    return negotiated_registry(session) >= MEISO_DISPLAY_PLANES_AUDIO_REGISTRY;
}

bool meiso_display_planes_configure_runtime(MeisoRuntimeV4Config *out, MeisoSession *session, uint32_t max_image_bytes)
{
    if (!out || !session || !max_image_bytes || max_image_bytes > MEISO_DISPLAY_PLANES_MAX_IMAGE_BYTES) return false;
    const bool input = meiso_display_planes_session_allows_input(session);
    const bool tracking = meiso_display_planes_session_allows_tracking(session);
    const bool extended = meiso_display_planes_session_allows_edge_extensions(session);
    const bool audio = meiso_display_planes_session_allows_audio(session);
    const uint32_t endpoints = 3U + tracking + input + extended + audio,
        routes = 11U + 4U * tracking + 3U * input + 3U * audio;
    MeisoRuntimeV4Config c = {0};
    c.base.session = session; c.base.max_endpoints = endpoints;
    c.base.max_endpoint_history = 2 * endpoints; c.base.max_routes = routes;
    c.base.max_capability_bytes = audio ? MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES : extended ? 352U : 208U;
    c.base.max_desired_profile_bytes_per_endpoint = MEISO_DISPLAY_PLANES_MAX_MODEL_BYTES;
    c.base.max_actual_profile_bytes_per_endpoint = 80;
    c.max_blob_transfer_bytes = max_image_bytes;
    if (!meiso_runtime_v4_memory_required(&c)) return false;
    *out = c; return true;
}

static MeisoRuntimeV4RouteDescriptor route(unsigned i, uint64_t incarnation, uint32_t max_image_bytes)
{
    static const MeisoResourceKind kinds[4] = {MEISO_RESOURCE_KIND_STATE, MEISO_RESOURCE_KIND_STATE,
        MEISO_RESOURCE_KIND_STREAM, MEISO_RESOURCE_KIND_BLOB};
    static const MeisoRuntimeFacet facets[4] = {MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA, MEISO_RUNTIME_FACET_IMMUTABLE};
    const uint32_t sizes[4] = {MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + MEISO_DISPLAY_PLANES_MAX_MODEL_BYTES,
        MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + MEISO_DISPLAY_PLANES_STATUS_BYTES,
        MEISO_DISPLAY_PLANES_EVIDENCE_BYTES, max_image_bytes};
    MeisoRuntimeV4RouteDescriptor v;
    memset(&v, 0, sizeof(v));
    v.route.endpoint_id = endpoint; v.route.endpoint_incarnation = incarnation;
    v.route.resource.id = (MeisoResourceId){DISPLAY_, resource_suffix[i]};
    v.route.resource.schema = (MeisoSchemaRef){{DISP_SCH, schema_suffix[i]}, 1};
    v.route.resource.kind = kinds[i];
    v.route.resource.owner = (i == 0 || i == 3) ? MEISO_OWNER_ROLE_HOST : MEISO_OWNER_ROLE_EDGE;
    v.route.resource.max_payload_size = sizes[i];
    v.route.facet = facets[i];
    v.delivery = MEISO_RUNTIME_DELIVERY_INLINE;
    return v;
}

static MeisoRuntimeV4RouteDescriptor tracking_route(unsigned i, uint64_t incarnation)
{
    static const MeisoResourceKind kinds[4] = {MEISO_RESOURCE_KIND_STATE, MEISO_RESOURCE_KIND_STATE,
        MEISO_RESOURCE_KIND_STREAM, MEISO_RESOURCE_KIND_STREAM};
    static const MeisoRuntimeFacet facets[4] = {MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA, MEISO_RUNTIME_FACET_DATA};
    static const MeisoOwnerRole owners[4] = {MEISO_OWNER_ROLE_HOST, MEISO_OWNER_ROLE_EDGE, MEISO_OWNER_ROLE_EDGE,
        MEISO_OWNER_ROLE_HOST};
    const uint32_t sizes[4] = {MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + MEISO_DISPLAY_TRACKING_DESIRED_BYTES,
        MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + MEISO_DISPLAY_TRACKING_ACTUAL_BYTES,
        MEISO_DISPLAY_TRACKING_MAX_IMU_BYTES, MEISO_DISPLAY_TRACKING_POSE_BYTES};
    MeisoRuntimeV4RouteDescriptor v;
    memset(&v, 0, sizeof(v));
    v.route.endpoint_id = *meiso_display_tracking_endpoint(); v.route.endpoint_incarnation = incarnation;
    v.route.resource.id = (MeisoResourceId){DISPLAY_, tracking_resource_suffix[i]};
    v.route.resource.schema = (MeisoSchemaRef){{DISP_SCH, tracking_schema_suffix[i]}, 1};
    v.route.resource.kind = kinds[i];
    v.route.resource.owner = owners[i];
    v.route.resource.max_payload_size = sizes[i];
    v.route.facet = facets[i];
    v.delivery = MEISO_RUNTIME_DELIVERY_INLINE;
    return v;
}

static MeisoRuntimeV4RouteDescriptor local_pose_route(unsigned i, uint64_t incarnation)
{
    /* The three resources have the same kinds/facets as input; all data is Edge-owned. */
    MeisoRuntimeV4RouteDescriptor v = {0};
    v.route.endpoint_id = *meiso_display_local_pose_endpoint(); v.route.endpoint_incarnation = incarnation;
    v.route.resource.id = (MeisoResourceId){DISPLAY_, local_pose_resource_suffix[i]};
    v.route.resource.schema = (MeisoSchemaRef){{DISP_SCH, local_pose_schema_suffix[i]}, 1};
    v.route.resource.kind = i == 2 ? MEISO_RESOURCE_KIND_STREAM : MEISO_RESOURCE_KIND_STATE;
    v.route.resource.owner = i == 0 ? MEISO_OWNER_ROLE_HOST : MEISO_OWNER_ROLE_EDGE;
    v.route.resource.max_payload_size = i == 0 ? MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + 16U :
        i == 1 ? MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + 16U : MEISO_DISPLAY_LOCAL_POSE_BYTES;
    v.route.facet = i == 0 ? MEISO_RUNTIME_FACET_DESIRED : i == 1 ? MEISO_RUNTIME_FACET_ACTUAL : MEISO_RUNTIME_FACET_DATA;
    v.delivery = MEISO_RUNTIME_DELIVERY_INLINE;
    return v;
}

static MeisoRuntimeV4RouteDescriptor input_route(unsigned i, uint64_t incarnation)
{
    static const MeisoResourceKind kinds[3] = {MEISO_RESOURCE_KIND_STATE, MEISO_RESOURCE_KIND_STATE,
        MEISO_RESOURCE_KIND_STREAM};
    static const MeisoRuntimeFacet facets[3] = {MEISO_RUNTIME_FACET_DESIRED, MEISO_RUNTIME_FACET_ACTUAL,
        MEISO_RUNTIME_FACET_DATA};
    static const MeisoOwnerRole owners[3] = {MEISO_OWNER_ROLE_HOST, MEISO_OWNER_ROLE_EDGE, MEISO_OWNER_ROLE_EDGE};
    const uint32_t sizes[3] = {MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + MEISO_DISPLAY_INPUT_DESIRED_BYTES,
        MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + MEISO_DISPLAY_INPUT_ACTUAL_BYTES, MEISO_DISPLAY_INPUT_KEY_BYTES};
    MeisoRuntimeV4RouteDescriptor v;
    memset(&v, 0, sizeof(v));
    v.route.endpoint_id = *meiso_display_input_endpoint(); v.route.endpoint_incarnation = incarnation;
    v.route.resource.id = (MeisoResourceId){DISPLAY_, input_resource_suffix[i]};
    v.route.resource.schema = (MeisoSchemaRef){{DISP_SCH, input_schema_suffix[i]}, 1};
    v.route.resource.kind = kinds[i];
    v.route.resource.owner = owners[i];
    v.route.resource.max_payload_size = sizes[i];
    v.route.facet = facets[i];
    v.delivery = MEISO_RUNTIME_DELIVERY_INLINE;
    return v;
}

static MeisoRuntimeV4RouteDescriptor audio_route(unsigned i, uint64_t incarnation)
{
    /* Same kinds, facets and owners as input's three. */
    MeisoRuntimeV4RouteDescriptor v = input_route(i, incarnation);
    v.route.endpoint_id = *meiso_display_audio_endpoint();
    v.route.resource.id = (MeisoResourceId){DISPLAY_, audio_resource_suffix[i]};
    v.route.resource.schema = (MeisoSchemaRef){{DISP_SCH, audio_schema_suffix[i]}, 1};
    v.route.resource.max_payload_size = i == 0 ? MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE + MEISO_DISPLAY_AUDIO_DESIRED_BYTES :
        i == 1 ? MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE + MEISO_DISPLAY_AUDIO_ACTUAL_BYTES : MEISO_DISPLAY_AUDIO_MAX_RECORD_BYTES;
    return v;
}

bool meiso_display_planes_build_catalog(const MeisoDisplayPlanesCatalogConfig *c, MeisoMutableBuffer storage,
    MeisoRuntimeEndpointDescriptor *ep, size_t ec, MeisoRuntimeV4RouteDescriptor *rt, size_t rc)
{
    if (!c || !ep || !rt || !storage.data || !cap_valid(&c->planes) ||
        c->video.capability.frame_format != MEISO_DISPLAY_INGRESS_FRAME_H264_IDR) return false;
    /* An Edge that takes the Host's head pose says so in its planes capability, and only then. */
    if ((c->tracking_enabled && c->local_pose_enabled) ||
        c->planes.tracking != (c->local_pose_enabled ? MEISO_DISPLAY_PLANES_TRACKING_EDGE :
            c->tracking_enabled ? MEISO_DISPLAY_PLANES_TRACKING_POSE : MEISO_DISPLAY_PLANES_TRACKING_NONE) ||
        c->views_enabled != (c->planes.view_layout == MEISO_DISPLAY_PLANES_VIEW_RUNTIME_VIEWS))
        return false;
    /* Audio records only while SYSTEM is held, so it comes with input that has SYSTEM (§15). */
    if (c->audio_enabled &&
        (!c->input_enabled || !(c->input.keys & meiso_display_input_key_bit(MEISO_DISPLAY_INPUT_KEY_SYSTEM)))) return false;
    const size_t endpoints = 2U + c->camera_enabled + c->tracking_enabled + c->local_pose_enabled + c->input_enabled +
        c->views_enabled + c->audio_enabled;
    const size_t routes = meiso_display_planes_audio_route_base(c) + 3U * c->audio_enabled;
    const size_t camera_bytes = 64U * c->camera_enabled;
    const size_t input_at = 96U + camera_bytes + 32U * c->tracking_enabled + 16U * c->local_pose_enabled;
    const size_t views_at = input_at + 16U * c->input_enabled;
    const size_t audio_at = views_at + MEISO_DISPLAY_VIEWS_CAPABILITY_BYTES * c->views_enabled;
    const size_t bytes = audio_at + MEISO_DISPLAY_AUDIO_CAPABILITY_BYTES * c->audio_enabled;
    if (ec < endpoints || rc < routes || storage.capacity < bytes) return false;
    const void *p[] = {c, ep, rt, storage.data};
    const size_t n[] = {sizeof(*c), endpoints * sizeof(*ep), routes * sizeof(*rt), bytes};
    for (unsigned i = 0; i < 4; ++i) for (unsigned j = i + 1; j < 4; ++j) if (overlap(p[i], n[i], p[j], n[j])) return false;
    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES] = {0};
    MeisoRuntimeEndpointDescriptor e[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS];
    MeisoRuntimeV4RouteDescriptor r[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    memset(e, 0, sizeof(e)); memset(r, 0, sizeof(r));
    if (meiso_display_ingress_private_build_catalog(&c->video, (MeisoMutableBuffer){cap, 32}, e, 1, r, 4, true) !=
        MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    e[0].capability = (MeisoByteView){storage.data, 32};
    if (!meiso_display_planes_capability_encode(&c->planes, (MeisoMutableBuffer){cap + 32, 64})) return false;
    e[1] = (MeisoRuntimeEndpointDescriptor){endpoint, c->video.endpoint_incarnation, capability_schema,
        {storage.data + 32, 64}};
    for (unsigned i = 0; i < 4; ++i) r[4 + i] = route(i, c->video.endpoint_incarnation, c->planes.max_image_bytes);
    if (c->camera_enabled) {
        size_t written = 0;
        if (c->camera.source != MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA ||
            meiso_composition_media_capability_encode(&c->camera, (MeisoMutableBuffer){cap + 96, 64}, &written) !=
                MEISO_COMPOSITION_PROFILE_OK)
            return false;
        e[2] = (MeisoRuntimeEndpointDescriptor){*meiso_composition_camera_media_endpoint(), c->video.endpoint_incarnation,
            *meiso_composition_media_capability_schema(), {storage.data + 96, 64}};
        for (unsigned i = 0; i < 3; ++i) r[8 + i] = meiso_display_camera_private_route(i, c->video.endpoint_incarnation);
    }
    if (c->tracking_enabled) {
        const size_t at = 96U + camera_bytes, index = 2U + c->camera_enabled, base = meiso_display_planes_tracking_route_base(c);
        if (!meiso_display_tracking_capability_encode(&c->tracking, (MeisoMutableBuffer){cap + at, 32})) return false;
        e[index] = (MeisoRuntimeEndpointDescriptor){*meiso_display_tracking_endpoint(), c->video.endpoint_incarnation,
            tracking_capability_schema, {storage.data + at, 32}};
        for (unsigned i = 0; i < 4; ++i) r[base + i] = tracking_route(i, c->video.endpoint_incarnation);
    }
    if (c->local_pose_enabled) {
        const size_t at = 96U + camera_bytes, index = 2U + c->camera_enabled, base = meiso_display_planes_local_pose_route_base(c);
        if (!meiso_display_local_pose_capability_encode(&c->local_pose, (MeisoMutableBuffer){cap + at, 16})) return false;
        e[index] = (MeisoRuntimeEndpointDescriptor){*meiso_display_local_pose_endpoint(), c->video.endpoint_incarnation,
            local_pose_capability_schema, {storage.data + at, 16}};
        for (unsigned i = 0; i < 3; ++i) r[base + i] = local_pose_route(i, c->video.endpoint_incarnation);
    }
    if (c->input_enabled) {
        const size_t index = 2U + c->camera_enabled + c->tracking_enabled + c->local_pose_enabled;
        const size_t base = meiso_display_planes_input_route_base(c);
        if (!meiso_display_input_capability_encode(&c->input, (MeisoMutableBuffer){cap + input_at, 16})) return false;
        e[index] = (MeisoRuntimeEndpointDescriptor){*meiso_display_input_endpoint(), c->video.endpoint_incarnation,
            input_capability_schema, {storage.data + input_at, 16}};
        for (unsigned i = 0; i < 3; ++i) r[base + i] = input_route(i, c->video.endpoint_incarnation);
    }
    if (c->views_enabled) {
        if (!meiso_display_views_capability_encode(&c->views, (MeisoMutableBuffer){cap + views_at, MEISO_DISPLAY_VIEWS_CAPABILITY_BYTES}))
            return false;
        e[endpoints - 1 - c->audio_enabled] = (MeisoRuntimeEndpointDescriptor){*meiso_display_views_endpoint(),
            c->video.endpoint_incarnation, views_capability_schema,
            {storage.data + views_at, MEISO_DISPLAY_VIEWS_CAPABILITY_BYTES}};
    }
    if (c->audio_enabled) {
        const size_t base = meiso_display_planes_audio_route_base(c);
        if (!meiso_display_audio_capability_encode(&c->audio,
                (MeisoMutableBuffer){cap + audio_at, MEISO_DISPLAY_AUDIO_CAPABILITY_BYTES})) return false;
        e[endpoints - 1] = (MeisoRuntimeEndpointDescriptor){*meiso_display_audio_endpoint(), c->video.endpoint_incarnation,
            audio_capability_schema, {storage.data + audio_at, MEISO_DISPLAY_AUDIO_CAPABILITY_BYTES}};
        for (unsigned i = 0; i < 3; ++i) r[base + i] = audio_route(i, c->video.endpoint_incarnation);
    }
    memcpy(storage.data, cap, bytes); memcpy(ep, e, endpoints * sizeof(*ep)); memcpy(rt, r, routes * sizeof(*rt));
    return true;
}

bool meiso_display_planes_validate_catalog(const MeisoRuntimeEndpointDescriptor *ep, size_t ec,
    const MeisoRuntimeV4RouteDescriptor *rt, size_t rc, MeisoDisplayPlanesCatalogConfig *out)
{
    if (!ep || !rt || !out || ec < 2 || ec > MEISO_DISPLAY_PLANES_MAX_ENDPOINTS || rc < 8 || rc > MEISO_DISPLAY_PLANES_MAX_ROUTES ||
        overlap(out, sizeof(*out), ep, ec * sizeof(*ep)) || overlap(out, sizeof(*out), rt, rc * sizeof(*rt))) return false;
    for (size_t i = 0; i < ec; ++i) if (overlap(out, sizeof(*out), ep[i].capability.data, ep[i].capability.size)) return false;
    MeisoDisplayPlanesCatalogConfig c;
    memset(&c, 0, sizeof(c));
    MeisoDisplayIngressCapability video;
    if (meiso_display_ingress_private_validate_catalog(ep, 1, rt, 4, &video, true) != MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    c.video.endpoint_incarnation = ep[0].incarnation; c.video.capability = video;
    if (!meiso_endpoint_id_equal(ep[1].id, endpoint) ||
        !meiso_display_planes_capability_decode(ep[1].capability, &c.planes)) return false;
    /* Optional endpoints in order: camera, tracking | local pose, input, views, audio; the rebuild below
     * checks each one's id. */
    size_t next = 2;
    const MeisoEndpointId *tracking = meiso_display_tracking_endpoint(), *input = meiso_display_input_endpoint();
    const MeisoEndpointId *local_pose = meiso_display_local_pose_endpoint(), *views = meiso_display_views_endpoint();
    const MeisoEndpointId *audio = meiso_display_audio_endpoint();
    if (next < ec && meiso_endpoint_id_equal(ep[next].id, *meiso_composition_camera_media_endpoint())) {
        c.camera_enabled = true;
        if (meiso_composition_media_capability_decode(ep[next].capability, &c.camera) != MEISO_COMPOSITION_PROFILE_OK) return false;
        ++next;
    }
    if (next < ec && meiso_endpoint_id_equal(ep[next].id, *tracking)) {
        c.tracking_enabled = true;
        if (!meiso_display_tracking_capability_decode(ep[next].capability, &c.tracking)) return false;
        ++next;
    }
    if (next < ec && meiso_endpoint_id_equal(ep[next].id, *local_pose)) {
        c.local_pose_enabled = true;
        if (!meiso_display_local_pose_capability_decode(ep[next].capability, &c.local_pose)) return false;
        ++next;
    }
    if (next < ec && meiso_endpoint_id_equal(ep[next].id, *input)) {
        c.input_enabled = true;
        if (!meiso_endpoint_id_equal(ep[next].id, *input) ||
            !meiso_display_input_capability_decode(ep[next].capability, &c.input)) return false;
        ++next;
    }
    if (next < ec && meiso_endpoint_id_equal(ep[next].id, *views)) {
        c.views_enabled = true;
        if (!meiso_display_views_capability_decode(ep[next].capability, &c.views)) return false;
        ++next;
    }
    if (next < ec && meiso_endpoint_id_equal(ep[next].id, *audio)) {
        c.audio_enabled = true;
        if (!meiso_display_audio_capability_decode(ep[next].capability, &c.audio)) return false;
        ++next;
    }
    if (next != ec || rc != meiso_display_planes_audio_route_base(&c) + 3U * c.audio_enabled) return false;
    uint8_t cap[MEISO_DISPLAY_PLANES_MAX_CAPABILITY_BYTES];
    MeisoRuntimeEndpointDescriptor e[MEISO_DISPLAY_PLANES_MAX_ENDPOINTS];
    MeisoRuntimeV4RouteDescriptor r[MEISO_DISPLAY_PLANES_MAX_ROUTES];
    if (!meiso_display_planes_build_catalog(&c, (MeisoMutableBuffer){cap, sizeof(cap)}, e, MEISO_DISPLAY_PLANES_MAX_ENDPOINTS, r,
            MEISO_DISPLAY_PLANES_MAX_ROUTES))
        return false;
    for (size_t i = 0; i < ec; ++i)
        if (!meiso_endpoint_id_equal(ep[i].id, e[i].id) || ep[i].incarnation != e[i].incarnation ||
            !meiso_schema_ref_equal(ep[i].capability_schema, e[i].capability_schema) ||
            ep[i].capability.size != e[i].capability.size || !ep[i].capability.data ||
            memcmp(ep[i].capability.data, e[i].capability.data, e[i].capability.size)) return false;
    for (size_t i = 0; i < rc; ++i)
        if (!meiso_endpoint_id_equal(rt[i].route.endpoint_id, r[i].route.endpoint_id) ||
            rt[i].route.endpoint_incarnation != r[i].route.endpoint_incarnation || rt[i].route.facet != r[i].route.facet ||
            rt[i].delivery != r[i].delivery || !meiso_resource_descriptor_equal(&rt[i].route.resource, &r[i].route.resource))
            return false;
    *out = c; return true;
}
