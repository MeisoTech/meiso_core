#include <meiso/profiles/display_ingress/input.h>
#include "display_ingress_profile_private.h"
#include <string.h>
#define put32 meiso_display_ingress_private_put_u32
#define put64 meiso_display_ingress_private_put_u64
#define get32 meiso_display_ingress_private_get_u32
#define get64 meiso_display_ingress_private_get_u64
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap

/* planes-v12.md §11: DISPLAY_/INPT_END. */
static const MeisoEndpointId endpoint = {UINT64_C(0x444953504c41595f), UINT64_C(0x494e50545f454e44)};

const MeisoEndpointId *meiso_display_input_endpoint(void) { return &endpoint; }

enum { ALL_KEYS = 0xfe }; /* bits 1..7: UP .. SYSTEM */

static bool cap_valid(const MeisoDisplayInputCapability *c)
{
    return c && c->keys && !(c->keys & ~(uint32_t)ALL_KEYS) && c->pointer <= MEISO_DISPLAY_INPUT_POINTER_HEAD_RAY;
}

bool meiso_display_input_capability_encode(const MeisoDisplayInputCapability *c, MeisoMutableBuffer out)
{
    if (!cap_valid(c) || !out.data || out.capacity < 16 || overlap(c, sizeof(*c), out.data, 16)) return false;
    uint8_t b[16] = {0};
    put32(b, c->keys); b[4] = c->pointer;
    memcpy(out.data, b, 16); return true;
}

bool meiso_display_input_capability_decode(MeisoByteView in, MeisoDisplayInputCapability *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 5, 11))
        return false;
    const MeisoDisplayInputCapability c = {get32(in.data), in.data[4]};
    if (!cap_valid(&c)) return false;
    *out = c; return true;
}

bool meiso_display_input_desired_encode(const MeisoDisplayInputDesired *d, MeisoMutableBuffer out)
{
    if (!d || !out.data || out.capacity < 16 || overlap(d, sizeof(*d), out.data, 16)) return false;
    memset(out.data, 0, 16);
    out.data[0] = d->keys_enabled;
    return true;
}

bool meiso_display_input_desired_decode(MeisoByteView in, MeisoDisplayInputDesired *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) || in.data[0] > 1 ||
        !zero(in.data + 1, 15)) return false;
    *out = (MeisoDisplayInputDesired){in.data[0] == 1};
    return true;
}

bool meiso_display_input_actual_encode(const MeisoDisplayInputActual *a, MeisoMutableBuffer out)
{
    if (!a || a->keys_state > MEISO_DISPLAY_INPUT_KEYS_RUNNING || !out.data || out.capacity < 16 ||
        overlap(a, sizeof(*a), out.data, 16)) return false;
    memset(out.data, 0, 16);
    out.data[0] = a->keys_state; out.data[1] = a->locked;
    return true;
}

bool meiso_display_input_actual_decode(MeisoByteView in, MeisoDisplayInputActual *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) ||
        in.data[0] > MEISO_DISPLAY_INPUT_KEYS_RUNNING || in.data[1] > 1 || !zero(in.data + 2, 14)) return false;
    *out = (MeisoDisplayInputActual){in.data[0], in.data[1] == 1};
    return true;
}

static bool key_valid(const MeisoDisplayInputCapability *c, const MeisoDisplayInputKeyEvent *k)
{
    return cap_valid(c) && k && k->time_ns && (c->keys & meiso_display_input_key_bit(k->key)) &&
        (k->action == MEISO_DISPLAY_INPUT_PRESS || k->action == MEISO_DISPLAY_INPUT_RELEASE) &&
        (k->target_plane ? k->target_generation && c->pointer == MEISO_DISPLAY_INPUT_POINTER_HEAD_RAY
                         : !k->target_generation);
}

bool meiso_display_input_key_encode(const MeisoDisplayInputCapability *c, const MeisoDisplayInputKeyEvent *k,
    MeisoMutableBuffer out)
{
    if (!key_valid(c, k) || !out.data || out.capacity < 24 || overlap(k, sizeof(*k), out.data, 24) ||
        overlap(c, sizeof(*c), out.data, 24)) return false;
    uint8_t b[24] = {0};
    put64(b, k->time_ns); b[8] = k->key; b[9] = k->action; put32(b + 12, k->target_plane); put64(b + 16, k->target_generation);
    memcpy(out.data, b, 24); return true;
}

bool meiso_display_input_key_decode(const MeisoDisplayInputCapability *c, MeisoByteView in, MeisoDisplayInputKeyEvent *out)
{
    if (!in.data || in.size != 24 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 10, 2))
        return false;
    const uint8_t *b = in.data;
    const MeisoDisplayInputKeyEvent k = {get64(b), b[8], b[9], get32(b + 12), get64(b + 16)};
    if (!key_valid(c, &k)) return false;
    *out = k; return true;
}
