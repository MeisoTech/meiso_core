#include <meiso/integrations/display_camera_channel.h>
#include <stdlib.h>
#include <string.h>

struct MeisoDisplayCameraChannel {
    MeisoDisplayCameraChannelConfig config;
    MeisoSessionRole role;
    MeisoSessionId session_id;
    MeisoOwnerScope host,edge;
    MeisoRuntimeRouteKey routes[3];
    MeisoCompositionMediaCapability capability;
    MeisoCompositionMediaDesired effective;
    MeisoCompositionMediaActual actual;
    MeisoCompositionMediaBinding binding;
    MeisoStateReplica *replica;
    MeisoCompositionMediaTransport *transport;
    MeisoCompositionMediaPosixTcp *tcp;
    void *replica_memory,*transport_memory,*tcp_memory;
    bool sent,received,failed;
    bool stopped,stop_published,capture_stop_failed;
    uint32_t terminal_reason;
    uint64_t actual_version;
    uint64_t submitted_sequence,producer_dropped;
};
static bool fail(MeisoDisplayCameraChannel *c) { c->failed=true; return false; }
static bool stop_media(MeisoDisplayCameraChannel *c)
{
    if(c->tcp && meiso_composition_media_posix_tcp_terminate(c->tcp,MEISO_COMPOSITION_MEDIA_TERMINAL_LOCAL_CLOSE,0)!=
        MEISO_COMPOSITION_MEDIA_POSIX_TCP_TERMINATE_OK) return fail(c);
    free(c->tcp_memory);free(c->transport_memory);
    c->tcp_memory=NULL;c->transport_memory=NULL;c->tcp=NULL;c->transport=NULL;
    return true;
}
static bool route_equal(MeisoRuntimeRouteKey a, MeisoRuntimeRouteKey b)
{
    return a.catalog_generation==b.catalog_generation && a.endpoint_incarnation==b.endpoint_incarnation &&
        a.facet==b.facet && meiso_endpoint_id_equal(a.endpoint_id,b.endpoint_id) && meiso_resource_id_equal(a.resource_id,b.resource_id);
}
static bool idle(MeisoDisplayCameraChannel *c)
{
    MeisoSessionSnapshot s;
    return meiso_session_snapshot(c->config.session,&s) && s.state==MEISO_SESSION_STATE_READY && !s.outbound_pending;
}
static bool same_desired(const MeisoCompositionMediaDesired *a,const MeisoCompositionMediaDesired *b)
{
    uint8_t x[32],y[32]; size_t n;
    return meiso_composition_media_desired_encode(a,(MeisoMutableBuffer){x,32},&n)==MEISO_COMPOSITION_PROFILE_OK &&
        meiso_composition_media_desired_encode(b,(MeisoMutableBuffer){y,32},&n)==MEISO_COMPOSITION_PROFILE_OK && !memcmp(x,y,32);
}
static bool arm(MeisoDisplayCameraChannel *c)
{
    MeisoRuntimeV4ExternalMediaBinding b;
    if(c->transport || meiso_runtime_v4_external_media_binding(c->config.runtime,c->routes[2].resource_id,&b)!=MEISO_RUNTIME_OK)
        return fail(c);
    c->binding=(MeisoCompositionMediaBinding){c->session_id,c->routes[2].catalog_generation,
        c->routes[2].endpoint_id,c->routes[2].endpoint_incarnation,c->routes[2].resource_id,
        MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,b.production_epoch,c->actual.media_stream_epoch,c->actual.channel_nonce};
    MeisoCompositionMediaTransportConfig cfg={0}; cfg.local_role=c->role;
    cfg.scope=(MeisoCompositionMediaOpen){c->session_id,c->routes[2].catalog_generation};
    cfg.active_route_mask=MEISO_COMPOSITION_MEDIA_TRANSPORT_ROUTE_CAMERA_MASK;
    cfg.routes[0].capability=(MeisoCompositionMediaCapability){.source=MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW,
        .max_access_unit_bytes=1,.max_fragment_payload_bytes=4096};
    cfg.routes[1]=(MeisoCompositionMediaRouteConfig){c->binding,c->capability,c->effective};
    size_t bytes,alignment;
    if(meiso_composition_media_transport_memory_required(&cfg,&bytes,&alignment)!=MEISO_COMPOSITION_MEDIA_TRANSPORT_OK ||
        bytes>1024*1024 || alignment>_Alignof(max_align_t)) return fail(c);
    c->transport_memory=malloc(bytes);
    if(!c->transport_memory || meiso_composition_media_transport_init(&cfg,
        (MeisoMutableBuffer){c->transport_memory,bytes},&c->transport)!=MEISO_COMPOSITION_MEDIA_TRANSPORT_OK) return fail(c);
    MeisoCompositionMediaPosixTcpConfig tcp={c->config.media_fd,c->transport,cfg.scope,4096};
    bytes=meiso_composition_media_posix_tcp_memory_required(&tcp);
    if(!bytes || bytes>16384) return fail(c);
    c->tcp_memory=malloc(bytes);
    return (c->tcp_memory && meiso_composition_media_posix_tcp_init(c->tcp_memory,bytes,&tcp,&c->tcp)==MEISO_COMPOSITION_MEDIA_POSIX_TCP_INIT_OK) || fail(c);
}
MeisoDisplayCameraChannel *meiso_display_camera_channel_open(const MeisoDisplayCameraChannelConfig *config)
{
    if(!config || !config->session || !config->runtime || config->media_fd<0) return NULL;
    MeisoSessionSnapshot session; MeisoRuntimeSnapshot runtime;
    if(!meiso_session_snapshot(config->session,&session) || session.state!=MEISO_SESSION_STATE_READY ||
        !meiso_runtime_snapshot(config->runtime,&runtime)) return NULL;
    MeisoDisplayCameraChannel *c=calloc(1,sizeof(*c)); if(!c) return NULL;
    c->config=*config; c->role=session.local_role; c->session_id=session.session_id;
    c->host=(MeisoOwnerScope){MEISO_OWNER_ROLE_HOST,c->role==MEISO_SESSION_ROLE_HOST?session.local_incarnation:session.remote_incarnation};
    c->edge=(MeisoOwnerScope){MEISO_OWNER_ROLE_EDGE,c->role==MEISO_SESSION_ROLE_EDGE?session.local_incarnation:session.remote_incarnation};
    MeisoRuntimeEndpointDescriptor ep;
    if(meiso_runtime_find_endpoint(config->runtime,*meiso_composition_camera_media_endpoint(),&ep)!=MEISO_RUNTIME_OK ||
        meiso_composition_media_capability_decode(ep.capability,&c->capability)!=MEISO_COMPOSITION_PROFILE_OK) goto error;
    for(unsigned i=0;i<3;++i) {
        const MeisoResourceDescriptor *r=&meiso_composition_camera_media_resources(NULL)[i];
        c->routes[i]=(MeisoRuntimeRouteKey){runtime.active_catalog_generation,ep.id,ep.incarnation,r->id,
            i==0?MEISO_RUNTIME_FACET_DESIRED:i==1?MEISO_RUNTIME_FACET_ACTUAL:MEISO_RUNTIME_FACET_DATA};
    }
    if(c->role==MEISO_SESSION_ROLE_HOST) {
        c->effective=config->desired;
        if(!meiso_composition_media_desired_supported(&c->capability,&c->effective)) goto error;
    } else if(!meiso_session_nonce_valid(config->edge_nonce)) goto error;
    const MeisoResourceDescriptor *r=&meiso_composition_camera_media_resources(NULL)[c->role==MEISO_SESSION_ROLE_HOST?1:0];
    const size_t bytes=meiso_state_replica_memory_required(r);
    if(!bytes || bytes>4096) goto error;
    c->replica_memory=malloc(bytes);
    if(!c->replica_memory || meiso_state_replica_init(c->replica_memory,bytes,r,&c->replica)!=MEISO_STATE_INIT_OK ||
        meiso_state_replica_begin_owner(c->replica,c->role==MEISO_SESSION_ROLE_HOST?c->edge:c->host)!=MEISO_STATE_OWNER_STARTED) goto error;
    return c;
error: meiso_display_camera_channel_close(c); return NULL;
}
int meiso_display_camera_channel_consume(MeisoDisplayCameraChannel *c,const MeisoRuntimeEvent *event)
{
    if(!c || !event || c->failed) return -1;
    if(!route_equal(event->route,c->routes[0]) && !route_equal(event->route,c->routes[1])) return 0;
    const bool host=c->role==MEISO_SESSION_ROLE_HOST;
    const unsigned receive_route=host?1:0;
    if(event->type==MEISO_RUNTIME_EVENT_STATE_ACCEPT) {
        if(c->sent && route_equal(event->route,c->routes[host?0:1]) &&
            event->state_accept_generation.value==(host?11U:12U) && event->state_accept_version &&
            event->state_accept_version<=(host?1:c->actual_version) &&
            meiso_owner_scope_equal(event->state_accept_owner,host?c->host:c->edge)) return 1;
        fail(c); return -1;
    }
    if(host && c->config.application_version==11 && c->received &&
        event->type==MEISO_RUNTIME_EVENT_STATE_OFFER && route_equal(event->route,c->routes[1]) &&
        event->state_offer.generation.value==12 && meiso_owner_scope_equal(event->state_offer.state.owner,c->edge)) {
        MeisoRuntimeActualState a;uint32_t reason;
        if(!meiso_runtime_actual_state_decode(event->state_offer.state.payload,&a) ||
            (a.production_status!=MEISO_RUNTIME_PRODUCTION_STOPPED && a.production_status!=MEISO_RUNTIME_PRODUCTION_FAILED) || a.effective_enabled || a.production_epoch ||
            a.apply_result!=(a.production_status==MEISO_RUNTIME_PRODUCTION_FAILED?MEISO_RUNTIME_APPLY_FAILED:MEISO_RUNTIME_APPLY_APPLIED) || a.availability!=MEISO_RUNTIME_AVAILABILITY_PRESENT ||
            a.reason!=(a.production_status==MEISO_RUNTIME_PRODUCTION_FAILED?MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED:MEISO_RUNTIME_PRODUCTION_REASON_NONE) || !a.applied_desired.present ||
            a.applied_desired.version!=1 || !meiso_owner_scope_equal(a.applied_desired.owner,c->host) || !a.observed_desired.present ||
            a.observed_desired.version!=1 || !meiso_owner_scope_equal(a.observed_desired.owner,c->host) ||
            !meiso_display_link_terminal_decode(a.profile,&reason) || reason!=MEISO_DISPLAY_LINK_LEASE_EXPIRED) {fail(c);return -1;}
        const uint64_t version=event->state_offer.state.version;
        const MeisoStateReceiveStatus r=meiso_state_replica_receive(c->replica,event->state_offer.state.owner,version,event->state_offer.state.payload);
        if(meiso_runtime_commit_state_receive(c->config.runtime,event->route,&event->state_offer,r)!=MEISO_RUNTIME_OK ||
            (r!=MEISO_STATE_RECEIVE_NEW && r!=MEISO_STATE_RECEIVE_DUPLICATE && r!=MEISO_STATE_RECEIVE_STALE)) {fail(c);return -1;}
        if(r==MEISO_STATE_RECEIVE_NEW) {
            if(!stop_media(c)) return -1;
            c->actual_version=version;c->terminal_reason=reason;c->stopped=c->stop_published=true;
            c->capture_stop_failed=a.production_status==MEISO_RUNTIME_PRODUCTION_FAILED;
        }
        return 1;
    }
    if(event->type!=MEISO_RUNTIME_EVENT_STATE_OFFER || c->received ||
        !route_equal(event->route,c->routes[receive_route]) || event->state_offer.state.version!=1 ||
        event->state_offer.generation.value!=(host?12U:11U) ||
        !meiso_owner_scope_equal(event->state_offer.state.owner,host?c->edge:c->host)) { fail(c); return -1; }
    if(host) {
        MeisoRuntimeActualState a;
        if(!c->sent || !meiso_runtime_actual_state_decode(event->state_offer.state.payload,&a) ||
            !a.effective_enabled || a.production_status!=MEISO_RUNTIME_PRODUCTION_RUNNING ||
            a.production_epoch!=1 || a.apply_result!=MEISO_RUNTIME_APPLY_APPLIED ||
            a.availability!=MEISO_RUNTIME_AVAILABILITY_PRESENT || a.reason!=MEISO_RUNTIME_PRODUCTION_REASON_NONE ||
            !a.observed_desired.present || a.observed_desired.version!=1 || !meiso_owner_scope_equal(a.observed_desired.owner,c->host) ||
            !a.applied_desired.present || a.applied_desired.version!=1 || !meiso_owner_scope_equal(a.applied_desired.owner,c->host) ||
            meiso_composition_media_actual_decode(a.profile,&c->actual)!=MEISO_COMPOSITION_PROFILE_OK ||
            c->actual.source!=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA || !same_desired(&c->actual.effective,&c->effective)) {fail(c);return -1;}
    } else {
        MeisoRuntimeDesiredState d;
        if(!meiso_runtime_desired_state_decode(event->state_offer.state.payload,&d) || !d.enabled ||
            meiso_composition_media_desired_decode(d.profile,&c->effective)!=MEISO_COMPOSITION_PROFILE_OK ||
            !meiso_composition_media_desired_supported(&c->capability,&c->effective)) {fail(c);return -1;}
    }
    const MeisoStateReceiveStatus received=meiso_state_replica_receive(c->replica,event->state_offer.state.owner,
        event->state_offer.state.version,event->state_offer.state.payload);
    if(received!=MEISO_STATE_RECEIVE_NEW || meiso_runtime_commit_state_receive(c->config.runtime,event->route,
        &event->state_offer,received)!=MEISO_RUNTIME_OK) {fail(c);return -1;}
    c->received=true;if(host)c->actual_version=event->state_offer.state.version;
    return !host || arm(c)?1:-1;
}
bool meiso_display_camera_channel_poll(MeisoDisplayCameraChannel *c)
{
    if(!c || c->failed) return false;
    const bool host=c->role==MEISO_SESSION_ROLE_HOST;
    if(c->stopped) {
        if(!host && !c->stop_published && idle(c)) {
            uint8_t suffix[8],wire[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE+8];MeisoByteView bytes;
            MeisoRuntimeActualState a={0};
            a.observed_desired=(MeisoRuntimeDesiredReference){true,c->host,1};a.applied_desired=a.observed_desired;
            a.apply_result=c->capture_stop_failed?MEISO_RUNTIME_APPLY_FAILED:MEISO_RUNTIME_APPLY_APPLIED;
            a.reason=c->capture_stop_failed?MEISO_RUNTIME_PRODUCTION_REASON_APPLY_FAILED:MEISO_RUNTIME_PRODUCTION_REASON_NONE;
            a.availability=MEISO_RUNTIME_AVAILABILITY_PRESENT;
            a.production_status=c->capture_stop_failed?MEISO_RUNTIME_PRODUCTION_FAILED:MEISO_RUNTIME_PRODUCTION_STOPPED;a.profile=(MeisoByteView){suffix,8};
            if(!meiso_display_link_terminal_encode(c->terminal_reason,(MeisoMutableBuffer){suffix,8}) ||
                !meiso_runtime_actual_state_encode(&a,(MeisoMutableBuffer){wire,sizeof(wire)},&bytes)) return fail(c);
            const MeisoStateOffer offer={{12},{c->edge,c->actual_version+1,bytes}};
            if(meiso_runtime_send_state_offer(c->config.runtime,c->routes[1],&offer)!=MEISO_RUNTIME_OK) return fail(c);
            ++c->actual_version;c->stop_published=true;
        }
        return true;
    }
    if(!c->sent && (host || c->received) && idle(c)) {
        uint8_t profile[80],wire[MEISO_COMPOSITION_MEDIA_ACTUAL_STATE_MAX_PAYLOAD_SIZE]; size_t size;
        MeisoByteView payload;
        if(host) {
            if(meiso_composition_media_desired_encode(&c->effective,(MeisoMutableBuffer){profile,80},&size)!=MEISO_COMPOSITION_PROFILE_OK) return fail(c);
            const MeisoRuntimeDesiredState d={true,{profile,size}};
            if(!meiso_runtime_desired_state_encode(&d,(MeisoMutableBuffer){wire,sizeof(wire)},&payload)) return fail(c);
        } else {
            c->actual=(MeisoCompositionMediaActual){.source=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,.effective=c->effective,
                .media_stream_epoch=1,.channel_nonce=c->config.edge_nonce};
            if(meiso_composition_media_actual_encode(&c->actual,(MeisoMutableBuffer){profile,80},&size)!=MEISO_COMPOSITION_PROFILE_OK) return fail(c);
            MeisoRuntimeActualState a={0};
            a.observed_desired=(MeisoRuntimeDesiredReference){true,c->host,1}; a.applied_desired=a.observed_desired;
            a.apply_result=MEISO_RUNTIME_APPLY_APPLIED; a.availability=MEISO_RUNTIME_AVAILABILITY_PRESENT;
            a.reason=MEISO_RUNTIME_PRODUCTION_REASON_NONE;
            a.effective_enabled=true; a.production_status=MEISO_RUNTIME_PRODUCTION_RUNNING; a.production_epoch=1;
            a.profile=(MeisoByteView){profile,size};
            if(!meiso_runtime_actual_state_encode(&a,(MeisoMutableBuffer){wire,sizeof(wire)},&payload)) return fail(c);
        }
        const MeisoStateOffer offer={{host?11U:12U},{host?c->host:c->edge,1,payload}};
        if(meiso_runtime_send_state_offer(c->config.runtime,c->routes[host?0:1],&offer)!=MEISO_RUNTIME_OK) return fail(c);
        c->sent=true; if(!host) {c->actual_version=1;if(!arm(c))return false;}
    }
    if(!c->tcp) return true;
    /* Bounded work: at most 16 packets per direction, ~64 KiB. */
    for(unsigned i=0;i<16;++i) {
        const MeisoCompositionMediaPosixTcpReadStatus r=meiso_composition_media_posix_tcp_on_readable(c->tcp);
        if(r==MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_WOULD_BLOCK || r==MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_INTERRUPTED) break;
        if(r>=MEISO_COMPOSITION_MEDIA_POSIX_TCP_READ_PACKET_REJECTED) return fail(c);
    }
    for(unsigned i=0;i<16;++i) {
        const MeisoCompositionMediaPosixTcpPrepareStatus p=meiso_composition_media_posix_tcp_prepare_outbound(c->tcp);
        if(p==MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_IDLE) break;
        if(p!=MEISO_COMPOSITION_MEDIA_POSIX_TCP_PREPARE_READY) return fail(c);
        const MeisoCompositionMediaPosixTcpWriteStatus w=meiso_composition_media_posix_tcp_on_writable(c->tcp);
        if(w==MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_WOULD_BLOCK || w==MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_INTERRUPTED) break;
        if(w>MEISO_COMPOSITION_MEDIA_POSIX_TCP_WRITE_IDLE) return fail(c);
    }
    return true;
}
bool meiso_display_camera_channel_ready(const MeisoDisplayCameraChannel *c)
{
    MeisoCompositionMediaTransportSnapshot s;
    return c && !c->failed && !c->stopped && c->transport && meiso_composition_media_transport_snapshot(c->transport,&s)==MEISO_COMPOSITION_MEDIA_TRANSPORT_OK &&
        (s.phase==MEISO_COMPOSITION_MEDIA_TRANSPORT_HOST_ACTIVE || s.phase==MEISO_COMPOSITION_MEDIA_TRANSPORT_EDGE_ACTIVE);
}
bool meiso_display_camera_channel_stop(MeisoDisplayCameraChannel *c,uint32_t reason,bool capture_stopped)
{
    if(!c || c->failed || c->config.application_version!=11 || c->role!=MEISO_SESSION_ROLE_EDGE ||
        !c->sent || reason!=MEISO_DISPLAY_LINK_LEASE_EXPIRED) return false;
    if(c->stopped) return c->terminal_reason==reason && c->capture_stop_failed==!capture_stopped;
    if(!stop_media(c)) return false;
    c->stopped=true;c->terminal_reason=reason;c->capture_stop_failed=!capture_stopped;return true;
}
bool meiso_display_camera_channel_stopped(const MeisoDisplayCameraChannel *c)
{ return c && !c->failed && c->stopped; }
bool meiso_display_camera_channel_stop_failed(const MeisoDisplayCameraChannel *c)
{ return c && !c->failed && c->stopped && c->capture_stop_failed; }
bool meiso_display_camera_channel_peer_eof(const MeisoDisplayCameraChannel *c)
{
    MeisoCompositionMediaPosixTcpSnapshot s;
    return c && c->failed && c->tcp && meiso_composition_media_posix_tcp_snapshot(c->tcp,&s) &&
        s.terminal && s.terminal_reason==MEISO_COMPOSITION_MEDIA_TERMINAL_MEDIA_TRANSPORT_LOST && s.last_error==0;
}
bool meiso_display_camera_channel_submit(MeisoDisplayCameraChannel *c,uint64_t sequence,MeisoTimePoint time,MeisoByteView pixels)
{
    if(!meiso_display_camera_channel_ready(c) || c->role!=MEISO_SESSION_ROLE_EDGE) return false;
    if(sequence<=c->submitted_sequence) return false;
    const uint64_t dropped=c->producer_dropped+sequence-c->submitted_sequence-1;
    const MeisoCompositionMediaUnit unit={.binding=c->binding,.frame_sequence=sequence,.source_time=time,
        .producer_dropped_frames=dropped,
        .pixel_format=c->effective.pixel_format,.codec=c->effective.codec,.width=c->effective.width,
        .height=c->effective.height,.flags=MEISO_COMPOSITION_MEDIA_FRAME_KEY,.access_unit=pixels};
    const MeisoCompositionMediaTransportStatus r=meiso_composition_media_transport_submit(c->transport,&unit);
    const bool accepted=r==MEISO_COMPOSITION_MEDIA_TRANSPORT_OK || r==MEISO_COMPOSITION_MEDIA_TRANSPORT_LATEST_REPLACED;
    if(accepted) {c->submitted_sequence=sequence;c->producer_dropped=dropped;}
    return accepted;
}
bool meiso_display_camera_channel_latest(MeisoDisplayCameraChannel *c,MeisoCompositionMediaLatestView *out)
{return c && !c->failed && c->transport && meiso_composition_media_transport_latest(c->transport,MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,out)==MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;}
bool meiso_display_camera_channel_release(MeisoDisplayCameraChannel *c,const MeisoCompositionMediaFrameRef *ref)
{return c && c->transport && meiso_composition_media_transport_release_latest(c->transport,ref)==MEISO_COMPOSITION_MEDIA_TRANSPORT_OK;}
const MeisoCompositionMediaDesired *meiso_display_camera_channel_effective(const MeisoDisplayCameraChannel *c)
{return c && c->received?&c->effective:NULL;}
void meiso_display_camera_channel_close(MeisoDisplayCameraChannel *c)
{
    if(!c) return;
    if(c->tcp) (void)meiso_composition_media_posix_tcp_terminate(c->tcp,MEISO_COMPOSITION_MEDIA_TERMINAL_CONTROL_SESSION,0);
    free(c->tcp_memory); free(c->transport_memory); free(c->replica_memory); free(c);
}
