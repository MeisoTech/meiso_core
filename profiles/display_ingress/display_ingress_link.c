#include <meiso/profiles/display_ingress/link.h>
#include "display_ingress_profile_private.h"
#include <string.h>

#define put32 meiso_display_ingress_private_put_u32
#define get32 meiso_display_ingress_private_get_u32
#define overlap meiso_display_ingress_private_ranges_overlap
static bool cap_valid(const MeisoDisplayLinkCapability *v)
{
    if (!v || !v->tier_mask || (v->tier_mask & ~7U) ||
        !v->decode_max_pixels || !v->render_max_pixels) return false;
    for (unsigned i=0; i<3; ++i) {
        if (v->tier_mask & (1U<<i)) {
            if (!v->downlink_kbit_s[i] || !v->uplink_kbit_s[i]) return false;
        } else if (v->downlink_kbit_s[i] || v->uplink_kbit_s[i]) return false;
    }
    return true;
}
static bool desired_valid(const MeisoDisplayLinkDesired *v)
{
    if (!v || v->tier>2 || !v->downlink_kbit_s || !v->uplink_kbit_s ||
        !v->keepalive_interval_ms || !v->keepalive_miss_limit) return false;
    const uint64_t lower=UINT64_C(2)*v->keepalive_interval_ms;
    const uint64_t upper=(uint64_t)v->keepalive_interval_ms*v->keepalive_miss_limit;
    for (unsigned i=0; i<3; ++i)
        if (v->lease_ms[i] && (v->lease_ms[i]<lower || v->lease_ms[i]>upper)) return false;
    return true;
}
bool meiso_display_link_capability_encode(const MeisoDisplayLinkCapability *v, MeisoMutableBuffer out)
{
    if (!cap_valid(v) || !out.data || out.capacity<40 || overlap(v,sizeof(*v),out.data,40)) return false;
    put32(out.data,v->tier_mask);
    for (unsigned i=0; i<3; ++i) {
        put32(out.data+4+8*i,v->downlink_kbit_s[i]);
        put32(out.data+8+8*i,v->uplink_kbit_s[i]);
    }
    put32(out.data+28,v->decode_max_pixels); put32(out.data+32,v->render_max_pixels);
    put32(out.data+36,0); return true;
}
bool meiso_display_link_capability_decode(MeisoByteView in, MeisoDisplayLinkCapability *out)
{
    if (!in.data || in.size!=40 || !out || overlap(in.data,40,out,sizeof(*out)) || get32(in.data+36)) return false;
    MeisoDisplayLinkCapability v={0}; v.tier_mask=get32(in.data);
    for (unsigned i=0; i<3; ++i) {
        v.downlink_kbit_s[i]=get32(in.data+4+8*i); v.uplink_kbit_s[i]=get32(in.data+8+8*i);
    }
    v.decode_max_pixels=get32(in.data+28); v.render_max_pixels=get32(in.data+32);
    if (!cap_valid(&v)) return false;
    *out=v; return true;
}
bool meiso_display_link_desired_encode(const MeisoDisplayLinkDesired *v, MeisoMutableBuffer out)
{
    if (!desired_valid(v) || !out.data || out.capacity<32 || overlap(v,sizeof(*v),out.data,32)) return false;
    put32(out.data,v->tier); put32(out.data+4,v->downlink_kbit_s); put32(out.data+8,v->uplink_kbit_s);
    put32(out.data+12,v->keepalive_interval_ms); put32(out.data+16,v->keepalive_miss_limit);
    for (unsigned i=0; i<3; ++i) put32(out.data+20+4*i,v->lease_ms[i]);
    return true;
}
bool meiso_display_link_desired_decode(MeisoByteView in, MeisoDisplayLinkDesired *out)
{
    if (!in.data || in.size!=32 || !out || overlap(in.data,32,out,sizeof(*out))) return false;
    MeisoDisplayLinkDesired v={0};
    v.tier=get32(in.data); v.downlink_kbit_s=get32(in.data+4); v.uplink_kbit_s=get32(in.data+8);
    v.keepalive_interval_ms=get32(in.data+12); v.keepalive_miss_limit=get32(in.data+16);
    for (unsigned i=0; i<3; ++i) v.lease_ms[i]=get32(in.data+20+4*i);
    if (!desired_valid(&v)) return false;
    *out=v; return true;
}
bool meiso_display_link_desired_supported(const MeisoDisplayLinkCapability *c, const MeisoDisplayLinkDesired *v)
{
    return cap_valid(c) && desired_valid(v) && (c->tier_mask & (1U<<v->tier)) &&
        v->downlink_kbit_s<=c->downlink_kbit_s[v->tier] && v->uplink_kbit_s<=c->uplink_kbit_s[v->tier];
}
bool meiso_display_link_terminal_encode(uint32_t reason, MeisoMutableBuffer out)
{
    if (reason>MEISO_DISPLAY_LINK_LEASE_EXPIRED || !out.data || out.capacity<8) return false;
    put32(out.data,reason); put32(out.data+4,0); return true;
}
bool meiso_display_link_terminal_decode(MeisoByteView in, uint32_t *reason)
{
    if (!in.data || in.size!=8 || !reason || overlap(in.data,8,reason,sizeof(*reason)) ||
        get32(in.data)>MEISO_DISPLAY_LINK_LEASE_EXPIRED || get32(in.data+4)) return false;
    *reason=get32(in.data); return true;
}
static const MeisoEndpointId endpoint={UINT64_C(0x444953504c41595f),UINT64_C(0x4c494e4b5f454e44)};
const MeisoEndpointId *meiso_display_link_endpoint_id(void) { return &endpoint; }
MeisoRuntimeEndpointDescriptor meiso_display_link_endpoint(uint64_t incarnation)
{
    const MeisoSchemaRef schema={{UINT64_C(0x444953505f534348),UINT64_C(0x4c494e4b5f434150)},1};
    return (MeisoRuntimeEndpointDescriptor){endpoint,incarnation,schema,{NULL,0}};
}
MeisoRuntimeV4RouteDescriptor meiso_display_link_route(unsigned i, uint64_t incarnation)
{
    MeisoRuntimeV4RouteDescriptor r={0};
    if (i>1) return r;
    const uint64_t id=i?UINT64_C(0x4c494e4b5f414354):UINT64_C(0x4c494e4b5f444553);
    r.route.endpoint_id=endpoint; r.route.endpoint_incarnation=incarnation;
    r.route.resource.id=(MeisoResourceId){UINT64_C(0x444953504c41595f),id};
    r.route.resource.schema=(MeisoSchemaRef){{UINT64_C(0x444953505f534348),id},1};
    r.route.resource.kind=MEISO_RESOURCE_KIND_STATE;
    r.route.resource.owner=i?MEISO_OWNER_ROLE_EDGE:MEISO_OWNER_ROLE_HOST;
    r.route.resource.max_payload_size=(i?MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE:MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE)+32;
    r.route.facet=i?MEISO_RUNTIME_FACET_ACTUAL:MEISO_RUNTIME_FACET_DESIRED;
    r.delivery=MEISO_RUNTIME_DELIVERY_INLINE; return r;
}
bool meiso_display_residence_accept(MeisoDisplayResidenceLease *v, uint64_t version, uint64_t now)
{
    if (!v || !version || version<=v->desired_version) return false;
    v->desired_version=version; v->renewed_ms=now; v->active=true; return true;
}
void meiso_display_residence_renew(MeisoDisplayResidenceLease *v, uint32_t lease, uint64_t now)
{
    if (!v) return;
    v->lease_ms=lease;
    if (v->active) v->renewed_ms=now;
}
bool meiso_display_residence_expired(const MeisoDisplayResidenceLease *v, uint64_t now)
{
    return v && v->active && v->lease_ms && now>=v->renewed_ms && now-v->renewed_ms>=v->lease_ms;
}
bool meiso_display_link_heartbeat_due(const MeisoDisplayLinkHeartbeat *v, uint64_t now)
{
    return v && v->interval_ms && v->miss_limit && !v->outstanding &&
        (!v->version || (now>=v->sent_ms && now-v->sent_ms>=v->interval_ms));
}
bool meiso_display_link_heartbeat_sent(MeisoDisplayLinkHeartbeat *v, uint64_t version, uint64_t now)
{
    if (!meiso_display_link_heartbeat_due(v,now) || !version || version<=v->version) return false;
    v->outstanding=v->version=version; v->sent_ms=now; return true;
}
bool meiso_display_link_heartbeat_observe(MeisoDisplayLinkHeartbeat *v, uint64_t version)
{
    if (!v || !version || version!=v->outstanding) return false;
    v->outstanding=0; return true;
}
bool meiso_display_link_heartbeat_lost(const MeisoDisplayLinkHeartbeat *v, uint64_t now)
{
    return v && v->outstanding && v->interval_ms && v->miss_limit && now>=v->sent_ms &&
        now-v->sent_ms>=(uint64_t)v->interval_ms*v->miss_limit;
}
