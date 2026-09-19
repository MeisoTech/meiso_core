#include <meiso/integrations/display_link_channel.h>
#include <stdlib.h>
#include <string.h>

struct MeisoDisplayLinkChannel {
    MeisoSession *session;
    MeisoRuntime *runtime;
    MeisoOwnerScope host, edge;
    MeisoRuntimeRouteKey routes[2];
    MeisoStateReplica *replica;
    void *memory;
    MeisoDisplayLinkCapability capability;
    MeisoDisplayLinkDesired desired;
    MeisoDisplayLinkHeartbeat heartbeat;
    uint64_t effective_version, sent_version;
    bool is_host, failed;
};
static bool route_equal(MeisoRuntimeRouteKey a, MeisoRuntimeRouteKey b)
{
    return a.catalog_generation==b.catalog_generation && a.endpoint_incarnation==b.endpoint_incarnation &&
        a.facet==b.facet && meiso_endpoint_id_equal(a.endpoint_id,b.endpoint_id) && meiso_resource_id_equal(a.resource_id,b.resource_id);
}
static bool fail(MeisoDisplayLinkChannel *c) { c->failed=true; return false; }
MeisoDisplayLinkChannel *meiso_display_link_channel_open(MeisoSession *session, MeisoRuntime *runtime,
    const MeisoDisplayLinkDesired *desired)
{
    MeisoSessionSnapshot ss; MeisoRuntimeSnapshot rs; MeisoRuntimeEndpointDescriptor ep;
    if (!session || !runtime || !meiso_session_snapshot(session,&ss) || ss.state!=MEISO_SESSION_STATE_READY ||
        !meiso_runtime_snapshot(runtime,&rs) || !rs.active_catalog_generation ||
        meiso_runtime_find_endpoint(runtime,*meiso_display_link_endpoint_id(),&ep)!=MEISO_RUNTIME_OK) return NULL;
    MeisoDisplayLinkChannel *c=calloc(1,sizeof(*c)); if (!c) return NULL;
    c->session=session; c->runtime=runtime; c->is_host=ss.local_role==MEISO_SESSION_ROLE_HOST;
    c->host=(MeisoOwnerScope){MEISO_OWNER_ROLE_HOST,c->is_host?ss.local_incarnation:ss.remote_incarnation};
    c->edge=(MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE,c->is_host?ss.remote_incarnation:ss.local_incarnation};
    const MeisoRuntimeEndpointDescriptor expected=meiso_display_link_endpoint(ep.incarnation);
    if (!meiso_schema_ref_equal(ep.capability_schema,expected.capability_schema) ||
        !meiso_display_link_capability_decode(ep.capability,&c->capability)) goto error;
    for (unsigned i=0;i<2;++i) {
        const MeisoRuntimeV4RouteDescriptor r=meiso_display_link_route(i,ep.incarnation);
        c->routes[i]=(MeisoRuntimeRouteKey){rs.active_catalog_generation,ep.id,ep.incarnation,r.route.resource.id,r.route.facet};
    }
    if (c->is_host) {
        if (!meiso_display_link_desired_supported(&c->capability,desired)) goto error;
        c->desired=*desired;
        c->heartbeat.interval_ms=desired->keepalive_interval_ms;c->heartbeat.miss_limit=desired->keepalive_miss_limit;
    }
    const MeisoRuntimeV4RouteDescriptor r=meiso_display_link_route(c->is_host?1:0,ep.incarnation);
    const size_t bytes=meiso_state_replica_memory_required(&r.route.resource);
    if (!bytes || bytes>4096) goto error;
    c->memory=malloc(bytes);
    if (!c->memory || meiso_state_replica_init(c->memory,bytes,&r.route.resource,&c->replica)!=MEISO_STATE_INIT_OK ||
        meiso_state_replica_begin_owner(c->replica,c->is_host?c->edge:c->host)!=MEISO_STATE_OWNER_STARTED) goto error;
    return c;
error:
    meiso_display_link_channel_close(c);return NULL;
}
int meiso_display_link_channel_consume(MeisoDisplayLinkChannel *c, const MeisoRuntimeEvent *event)
{
    if (!c || !event || c->failed) return -1;
    if (!route_equal(event->route,c->routes[0]) && !route_equal(event->route,c->routes[1])) return 0;
    const unsigned incoming=c->is_host?1:0;
    if (event->type==MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
        if (route_equal(event->route,c->routes[1-incoming]) && event->state_accept_generation.value==(c->is_host?13U:14U) &&
            event->state_accept_version && event->state_accept_version<=c->sent_version &&
            meiso_owner_scope_equal(event->state_accept_owner,c->is_host?c->host:c->edge)) return 1;
        goto error;
    }
    if (event->type!=MEISO_RUNTIME_EVENT_STATE_OFFER || !route_equal(event->route,c->routes[incoming]) ||
        event->state_offer.generation.value!=(c->is_host?14U:13U) ||
        !meiso_owner_scope_equal(event->state_offer.state.owner,c->is_host?c->edge:c->host)) goto error;
    const uint64_t version=event->state_offer.state.version;
    MeisoDisplayLinkDesired value;
    if (c->is_host) {
        MeisoRuntimeActualState a; uint8_t expected[MEISO_DISPLAY_LINK_DESIRED_BYTES];
        if (!version || version>c->sent_version || !meiso_runtime_actual_state_decode(event->state_offer.state.payload,&a) ||
            a.production_status!=MEISO_RUNTIME_PRODUCTION_RUNNING || !a.effective_enabled || a.production_epoch!=version ||
            a.availability!=MEISO_RUNTIME_AVAILABILITY_PRESENT || a.apply_result!=MEISO_RUNTIME_APPLY_APPLIED ||
            a.reason!=MEISO_RUNTIME_PRODUCTION_REASON_NONE || !a.observed_desired.present ||
            a.observed_desired.version!=version || !meiso_owner_scope_equal(a.observed_desired.owner,c->host) ||
            !meiso_display_link_desired_decode(a.profile,&value) ||
            !meiso_display_link_desired_encode(&c->desired,(MeisoMutableBuffer){expected,MEISO_DISPLAY_LINK_DESIRED_BYTES}) || memcmp(expected,a.profile.data,MEISO_DISPLAY_LINK_DESIRED_BYTES)) goto error;
        /* Runtime APPLIED validates applied==observed. */
    } else {
        MeisoRuntimeDesiredState d;
        if (!meiso_runtime_desired_state_decode(event->state_offer.state.payload,&d) || !d.enabled ||
            !meiso_display_link_desired_decode(d.profile,&value) ||
            !meiso_display_link_desired_supported(&c->capability,&value)) goto error;
    }
    const MeisoStateReceiveStatus r=meiso_state_replica_receive(c->replica,event->state_offer.state.owner,version,event->state_offer.state.payload);
    if (meiso_runtime_commit_state_receive(c->runtime,event->route,&event->state_offer,r)!=MEISO_RUNTIME_OK) goto error;
    if (r==MEISO_STATE_RECEIVE_DUPLICATE || r==MEISO_STATE_RECEIVE_STALE) return 1;
    if (r!=MEISO_STATE_RECEIVE_NEW) goto error;
    if (c->is_host && !meiso_display_link_heartbeat_observe(&c->heartbeat,version)) goto error;
    c->desired=value;c->effective_version=version;return 1;
error:
    (void)fail(c);return -1;
}
bool meiso_display_link_channel_poll(MeisoDisplayLinkChannel *c, uint64_t now)
{
    if (!c || c->failed) return false;
    if (c->is_host && meiso_display_link_heartbeat_lost(&c->heartbeat,now)) {
        meiso_session_transport_lost(c->session);return fail(c);
    }
    MeisoSessionSnapshot ss;
    if (!meiso_session_snapshot(c->session,&ss) || ss.state!=MEISO_SESSION_STATE_READY) return fail(c);
    if (ss.outbound_pending) return true;
    if (c->is_host?!meiso_display_link_heartbeat_due(&c->heartbeat,now):c->sent_version==c->effective_version) return true;
    if (c->is_host && c->sent_version==UINT64_MAX) return fail(c);
    const uint64_t version=c->is_host?c->sent_version+1:c->effective_version;
    uint8_t profile[MEISO_DISPLAY_LINK_DESIRED_BYTES],wire[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE+MEISO_DISPLAY_LINK_DESIRED_BYTES];MeisoByteView bytes;
    if (!meiso_display_link_desired_encode(&c->desired,(MeisoMutableBuffer){profile,MEISO_DISPLAY_LINK_DESIRED_BYTES})) return fail(c);
    if (c->is_host) {
        const MeisoRuntimeDesiredState d={true,{profile,MEISO_DISPLAY_LINK_DESIRED_BYTES}};
        if (!meiso_runtime_desired_state_encode(&d,(MeisoMutableBuffer){wire,sizeof(wire)},&bytes)) return fail(c);
    } else {
        MeisoRuntimeActualState a={0};
        a.observed_desired=(MeisoRuntimeDesiredReference){true,c->host,version};a.applied_desired=a.observed_desired;
        a.apply_result=MEISO_RUNTIME_APPLY_APPLIED;a.availability=MEISO_RUNTIME_AVAILABILITY_PRESENT;
        a.effective_enabled=true;a.production_status=MEISO_RUNTIME_PRODUCTION_RUNNING;a.production_epoch=version;
        a.profile=(MeisoByteView){profile,MEISO_DISPLAY_LINK_DESIRED_BYTES};
        if (!meiso_runtime_actual_state_encode(&a,(MeisoMutableBuffer){wire,sizeof(wire)},&bytes)) return fail(c);
    }
    const MeisoStateOffer offer={{c->is_host?13U:14U},{c->is_host?c->host:c->edge,version,bytes}};
    if (meiso_runtime_send_state_offer(c->runtime,c->routes[c->is_host?0:1],&offer)!=MEISO_RUNTIME_OK) return fail(c);
    if (c->is_host && !meiso_display_link_heartbeat_sent(&c->heartbeat,version,now)) return fail(c);
    c->sent_version=version;return true;
}
uint64_t meiso_display_link_channel_effective(const MeisoDisplayLinkChannel *c, MeisoDisplayLinkDesired *out)
{
    if (!c || c->failed || !out || !c->effective_version) return 0;
    *out=c->desired;return c->effective_version;
}
void meiso_display_link_channel_close(MeisoDisplayLinkChannel *c)
{ if (c) { free(c->memory);free(c); } }
