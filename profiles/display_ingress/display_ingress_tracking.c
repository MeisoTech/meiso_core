#include <meiso/profiles/display_ingress/tracking.h>
#include "display_ingress_profile_private.h"
#include <math.h>
#include <string.h>
#define put32 meiso_display_ingress_private_put_u32
#define put64 meiso_display_ingress_private_put_u64
#define get32 meiso_display_ingress_private_get_u32
#define get64 meiso_display_ingress_private_get_u64
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap

/* planes-v12.md §10: DISPLAY_/TRCK_END. */
static const MeisoEndpointId endpoint = {UINT64_C(0x444953504c41595f), UINT64_C(0x5452434b5f454e44)};

const MeisoEndpointId *meiso_display_tracking_endpoint(void) { return &endpoint; }

static void putf(uint8_t *b, float v) { uint32_t u; memcpy(&u, &v, 4); put32(b, u); }
static float getf(const uint8_t *b) { uint32_t u = get32(b); float v; memcpy(&v, &u, 4); return v; }
static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }
static uint16_t get16(const uint8_t *b) { return (uint16_t)(b[0] * 256U + b[1]); }
static bool within(float v, float limit) { return isfinite(v) && v >= -limit && v <= limit; }

static bool cap_valid(const MeisoDisplayTrackingCapability *c)
{
    if (!c || !c->max_imu_batch || c->max_imu_batch > MEISO_DISPLAY_TRACKING_MAX_IMU_BATCH ||
        c->correction > MEISO_DISPLAY_TRACKING_CORRECTION_IMU || !c->max_prediction_ms ||
        c->max_prediction_ms > MEISO_DISPLAY_TRACKING_MAX_PREDICTION_MS) return false;
    if (!c->imu_rate_millihz)
        return c->accel_range_mps2 == 0.0f && !signbit(c->accel_range_mps2) && c->gyro_range_rps == 0.0f &&
            !signbit(c->gyro_range_rps) && c->correction == MEISO_DISPLAY_TRACKING_CORRECTION_NONE;
    return c->imu_rate_millihz >= 50000U && c->imu_rate_millihz <= 2000000U && isfinite(c->accel_range_mps2) &&
        c->accel_range_mps2 > 0.0f && c->accel_range_mps2 <= 2000.0f && isfinite(c->gyro_range_rps) &&
        c->gyro_range_rps > 0.0f && c->gyro_range_rps <= 200.0f;
}

bool meiso_display_tracking_capability_encode(const MeisoDisplayTrackingCapability *c, MeisoMutableBuffer out)
{
    if (!cap_valid(c) || !out.data || out.capacity < 32 || overlap(c, sizeof(*c), out.data, 32)) return false;
    uint8_t b[32] = {0};
    put32(b, c->imu_rate_millihz); putf(b + 4, c->accel_range_mps2); putf(b + 8, c->gyro_range_rps);
    b[12] = c->max_imu_batch; b[13] = c->correction; put16(b + 14, c->max_prediction_ms);
    memcpy(out.data, b, 32); return true;
}

bool meiso_display_tracking_capability_decode(MeisoByteView in, MeisoDisplayTrackingCapability *out)
{
    if (!in.data || in.size != 32 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 16, 16))
        return false;
    const uint8_t *b = in.data;
    MeisoDisplayTrackingCapability c = {get32(b), getf(b + 4), getf(b + 8), b[12], b[13], get16(b + 14)};
    if (!cap_valid(&c)) return false;
    *out = c; return true;
}

bool meiso_display_tracking_desired_encode(const MeisoDisplayTrackingDesired *d, MeisoMutableBuffer out)
{
    if (!d || !out.data || out.capacity < 16 || overlap(d, sizeof(*d), out.data, 16)) return false;
    memset(out.data, 0, 16);
    out.data[0] = d->imu_enabled; out.data[1] = d->pose_enabled;
    return true;
}

bool meiso_display_tracking_desired_decode(MeisoByteView in, MeisoDisplayTrackingDesired *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) || in.data[0] > 1 ||
        in.data[1] > 1 || !zero(in.data + 2, 14)) return false;
    *out = (MeisoDisplayTrackingDesired){in.data[0] == 1, in.data[1] == 1};
    return true;
}

bool meiso_display_tracking_desired_supported(const MeisoDisplayTrackingCapability *c, const MeisoDisplayTrackingDesired *d)
{
    return cap_valid(c) && d && (!d->imu_enabled || c->imu_rate_millihz);
}

bool meiso_display_tracking_actual_encode(const MeisoDisplayTrackingActual *a, MeisoMutableBuffer out)
{
    if (!a || a->imu_state > MEISO_DISPLAY_TRACKING_IMU_FAILED || a->pose_state > MEISO_DISPLAY_TRACKING_POSE_HOLDING ||
        !out.data || out.capacity < 16 || overlap(a, sizeof(*a), out.data, 16)) return false;
    memset(out.data, 0, 16);
    out.data[0] = a->imu_state; out.data[1] = a->pose_state;
    return true;
}

bool meiso_display_tracking_actual_decode(MeisoByteView in, MeisoDisplayTrackingActual *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) ||
        in.data[0] > MEISO_DISPLAY_TRACKING_IMU_FAILED || in.data[1] > MEISO_DISPLAY_TRACKING_POSE_HOLDING ||
        !zero(in.data + 2, 14)) return false;
    *out = (MeisoDisplayTrackingActual){in.data[0], in.data[1]};
    return true;
}

/* Times strictly increase; values finite and within 1.1 × full scale. */
static bool imu_valid(const MeisoDisplayTrackingCapability *c, const MeisoDisplayTrackingImu *m)
{
    if (!cap_valid(c) || !c->imu_rate_millihz || !m || !m->first_sequence || !m->count || m->count > c->max_imu_batch ||
        m->first_sequence > UINT64_MAX - m->count) return false;
    const float accel = c->accel_range_mps2 * 1.1f, gyro = c->gyro_range_rps * 1.1f;
    for (uint8_t i = 0; i < m->count; ++i) {
        const MeisoDisplayTrackingImuSample *s = &m->samples[i];
        if (!s->time_ns || (i && s->time_ns <= m->samples[i - 1].time_ns)) return false;
        for (int k = 0; k < 3; ++k) if (!within(s->accel_mps2[k], accel) || !within(s->gyro_rps[k], gyro)) return false;
    }
    return true;
}

size_t meiso_display_tracking_imu_encode(const MeisoDisplayTrackingCapability *c, const MeisoDisplayTrackingImu *m,
    MeisoMutableBuffer out)
{
    if (!imu_valid(c, m)) return 0;
    const size_t bytes = 16U + 32U * m->count;
    if (!out.data || out.capacity < bytes || overlap(m, sizeof(*m), out.data, bytes) || overlap(c, sizeof(*c), out.data, bytes))
        return 0;
    uint8_t b[MEISO_DISPLAY_TRACKING_MAX_IMU_BYTES] = {0};
    put64(b, m->first_sequence); b[8] = m->count;
    for (uint8_t i = 0; i < m->count; ++i) {
        uint8_t *p = b + 16 + 32 * i;
        put64(p, m->samples[i].time_ns);
        for (int k = 0; k < 3; ++k) { putf(p + 8 + 4 * k, m->samples[i].accel_mps2[k]); putf(p + 20 + 4 * k, m->samples[i].gyro_rps[k]); }
    }
    memcpy(out.data, b, bytes);
    return bytes;
}

bool meiso_display_tracking_imu_decode(const MeisoDisplayTrackingCapability *c, MeisoByteView in, MeisoDisplayTrackingImu *out)
{
    if (!in.data || in.size < 48 || (in.size - 16) % 32 || in.size > MEISO_DISPLAY_TRACKING_MAX_IMU_BYTES || !out ||
        overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 9, 7) || in.data[8] != (in.size - 16) / 32) return false;
    MeisoDisplayTrackingImu m;
    memset(&m, 0, sizeof(m));
    m.first_sequence = get64(in.data); m.count = in.data[8];
    for (uint8_t i = 0; i < m.count; ++i) {
        const uint8_t *p = in.data + 16 + 32 * i;
        m.samples[i].time_ns = get64(p);
        for (int k = 0; k < 3; ++k) { m.samples[i].accel_mps2[k] = getf(p + 8 + 4 * k); m.samples[i].gyro_rps[k] = getf(p + 20 + 4 * k); }
    }
    if (!imu_valid(c, &m)) return false;
    *out = m; return true;
}

static bool pose_valid(const MeisoDisplayTrackingPose *p)
{
    if (!p || !p->time_ns || !p->world_epoch ||
        (p->quality != MEISO_DISPLAY_TRACKING_TRACKING && p->quality != MEISO_DISPLAY_TRACKING_LOST)) return false;
    const float *q = p->orientation;
    for (int i = 0; i < 3; ++i)
        if (!within(p->position[i], 1000.0f) || !within(p->velocity[i], 20.0f) || !within(p->angular[i], 35.0f)) return false;
    for (int i = 0; i < 4; ++i) if (!within(q[i], 1.001f)) return false;
    const float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    return n >= 0.999f && n <= 1.001f;
}

bool meiso_display_tracking_pose_encode(const MeisoDisplayTrackingPose *p, MeisoMutableBuffer out)
{
    if (!pose_valid(p) || !out.data || out.capacity < 72 || overlap(p, sizeof(*p), out.data, 72)) return false;
    uint8_t b[72] = {0};
    put64(b, p->time_ns); put32(b + 8, p->world_epoch); b[12] = p->quality;
    for (int i = 0; i < 3; ++i) { putf(b + 16 + 4 * i, p->position[i]); putf(b + 44 + 4 * i, p->velocity[i]); putf(b + 56 + 4 * i, p->angular[i]); }
    for (int i = 0; i < 4; ++i) putf(b + 28 + 4 * i, p->orientation[i]);
    memcpy(out.data, b, 72); return true;
}

bool meiso_display_tracking_pose_decode(MeisoByteView in, MeisoDisplayTrackingPose *out)
{
    if (!in.data || in.size != 72 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 13, 3) ||
        !zero(in.data + 68, 4)) return false;
    const uint8_t *b = in.data;
    MeisoDisplayTrackingPose p;
    memset(&p, 0, sizeof(p));
    p.time_ns = get64(b); p.world_epoch = get32(b + 8); p.quality = b[12];
    for (int i = 0; i < 3; ++i) { p.position[i] = getf(b + 16 + 4 * i); p.velocity[i] = getf(b + 44 + 4 * i); p.angular[i] = getf(b + 56 + 4 * i); }
    for (int i = 0; i < 4; ++i) p.orientation[i] = getf(b + 28 + 4 * i);
    if (!pose_valid(&p)) return false;
    *out = p; return true;
}
