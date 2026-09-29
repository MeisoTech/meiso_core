#include <meiso/profiles/display_ingress/local_pose.h>
#include "display_ingress_profile_private.h"
#include <string.h>
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap
static const MeisoEndpointId endpoint = {UINT64_C(0x444953504c41595f), UINT64_C(0x4c504f5345454e44)};
const MeisoEndpointId *meiso_display_local_pose_endpoint(void) { return &endpoint; }

bool meiso_display_local_pose_capability_encode(const MeisoDisplayLocalPoseCapability *c, MeisoMutableBuffer out)
{
    if (!c || !c->max_samples_per_second || c->max_samples_per_second > MEISO_DISPLAY_LOCAL_POSE_MAX_RATE ||
        !out.data || out.capacity < 16 || overlap(c, sizeof(*c), out.data, 16)) return false;
    uint8_t b[16] = {0};
    b[0] = (uint8_t)(c->max_samples_per_second >> 8); b[1] = (uint8_t)c->max_samples_per_second;
    memcpy(out.data, b, 16); return true;
}
bool meiso_display_local_pose_capability_decode(MeisoByteView in, MeisoDisplayLocalPoseCapability *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) || !zero(in.data + 2, 14))
        return false;
    const MeisoDisplayLocalPoseCapability c = {(uint16_t)(in.data[0] * 256U + in.data[1])};
    if (!c.max_samples_per_second || c.max_samples_per_second > MEISO_DISPLAY_LOCAL_POSE_MAX_RATE) return false;
    *out = c; return true;
}
bool meiso_display_local_pose_desired_encode(const MeisoDisplayLocalPoseDesired *d, MeisoMutableBuffer out)
{
    if (!d || !out.data || out.capacity < 16 || overlap(d, sizeof(*d), out.data, 16)) return false;
    memset(out.data, 0, 16); out.data[0] = d->enabled; return true;
}
bool meiso_display_local_pose_desired_decode(MeisoByteView in, MeisoDisplayLocalPoseDesired *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) ||
        in.data[0] > 1 || !zero(in.data + 1, 15)) return false;
    *out = (MeisoDisplayLocalPoseDesired){in.data[0] == 1}; return true;
}
bool meiso_display_local_pose_actual_encode(const MeisoDisplayLocalPoseActual *a, MeisoMutableBuffer out)
{
    if (!a || a->state > MEISO_DISPLAY_LOCAL_POSE_FAILED || !out.data || out.capacity < 16 ||
        overlap(a, sizeof(*a), out.data, 16)) return false;
    memset(out.data, 0, 16); out.data[0] = a->state; return true;
}
bool meiso_display_local_pose_actual_decode(MeisoByteView in, MeisoDisplayLocalPoseActual *out)
{
    if (!in.data || in.size != 16 || !out || overlap(in.data, in.size, out, sizeof(*out)) ||
        in.data[0] > MEISO_DISPLAY_LOCAL_POSE_FAILED || !zero(in.data + 1, 15)) return false;
    *out = (MeisoDisplayLocalPoseActual){in.data[0]}; return true;
}
bool meiso_display_local_pose_encode(const MeisoDisplayLocalPose *p, MeisoMutableBuffer out)
{ return meiso_display_tracking_pose_encode(p, out); }
bool meiso_display_local_pose_decode(MeisoByteView in, MeisoDisplayLocalPose *out)
{ return meiso_display_tracking_pose_decode(in, out); }
