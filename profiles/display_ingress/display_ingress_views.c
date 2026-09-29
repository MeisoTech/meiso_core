#include <meiso/profiles/display_ingress/views.h>
#include "display_ingress_profile_private.h"
#include <math.h>
#include <string.h>
#define put32 meiso_display_ingress_private_put_u32
#define get32 meiso_display_ingress_private_get_u32
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap
static const MeisoEndpointId endpoint = {UINT64_C(0x444953504c41595f), UINT64_C(0x5649455753454e44)};
const MeisoEndpointId *meiso_display_views_endpoint(void) { return &endpoint; }
static void putf(uint8_t *b, float v) { uint32_t u; memcpy(&u, &v, 4); put32(b, u); }
static float getf(const uint8_t *b) { uint32_t u = get32(b); float v; memcpy(&v, &u, 4); return v; }
static bool valid(const MeisoDisplayViewsCapability *c)
{
    const float h = 1.5707963f;
    if (!c || c->count < 1 || c->count > MEISO_DISPLAY_VIEWS_MAX_COUNT) return false;
    for (unsigned i = 0; i < c->count; ++i) {
        const MeisoDisplayView *v = &c->views[i];
        if (!v->recommended_width || !v->recommended_height ||
            !isfinite(v->fov_left) || !isfinite(v->fov_right) || !isfinite(v->fov_up) || !isfinite(v->fov_down) ||
            !(v->fov_left > -h && v->fov_left < v->fov_right && v->fov_right < h) ||
            !(v->fov_down > -h && v->fov_down < v->fov_up && v->fov_up < h)) return false;
        for (unsigned k = 0; k < 3; ++k)
            if (!isfinite(v->position[k]) || fabsf(v->position[k]) > 1.0f) return false;
        float n = 0;
        for (unsigned k = 0; k < 4; ++k) {
            if (!isfinite(v->orientation[k]) || fabsf(v->orientation[k]) > 1.001f) return false;
            n += v->orientation[k] * v->orientation[k];
        }
        if (n < 0.999f || n > 1.001f) return false;
    }
    return true;
}
bool meiso_display_views_capability_encode(const MeisoDisplayViewsCapability *c, MeisoMutableBuffer out)
{
    if (!valid(c) || !out.data || out.capacity < 144 || overlap(c, sizeof(*c), out.data, 144)) return false;
    uint8_t b[144] = {0}; b[0] = c->count;
    for (unsigned i = 0; i < c->count; ++i) {
        const MeisoDisplayView *v = &c->views[i]; uint8_t *s = b + 16 + i * 64;
        put32(s, v->recommended_width); put32(s + 4, v->recommended_height);
        putf(s + 8, v->fov_left); putf(s + 12, v->fov_right); putf(s + 16, v->fov_up); putf(s + 20, v->fov_down);
        for (unsigned k = 0; k < 3; ++k) putf(s + 24 + k * 4, v->position[k]);
        for (unsigned k = 0; k < 4; ++k) putf(s + 36 + k * 4, v->orientation[k]);
    }
    memcpy(out.data, b, 144); return true;
}
bool meiso_display_views_capability_decode(MeisoByteView in, MeisoDisplayViewsCapability *out)
{
    if (!in.data || in.size != 144 || !out || overlap(in.data, in.size, out, sizeof(*out)) ||
        in.data[0] < 1 || in.data[0] > 2 || !zero(in.data + 1, 15)) return false;
    MeisoDisplayViewsCapability c = {0}; c.count = in.data[0];
    for (unsigned i = 0; i < 2; ++i) {
        const uint8_t *s = in.data + 16 + i * 64;
        if (i >= c.count) { if (!zero(s, 64)) return false; continue; }
        if (!zero(s + 52, 12)) return false;
        MeisoDisplayView *v = &c.views[i];
        v->recommended_width = get32(s); v->recommended_height = get32(s + 4);
        v->fov_left = getf(s + 8); v->fov_right = getf(s + 12); v->fov_up = getf(s + 16); v->fov_down = getf(s + 20);
        for (unsigned k = 0; k < 3; ++k) v->position[k] = getf(s + 24 + k * 4);
        for (unsigned k = 0; k < 4; ++k) v->orientation[k] = getf(s + 36 + k * 4);
    }
    if (!valid(&c)) return false;
    *out = c; return true;
}
