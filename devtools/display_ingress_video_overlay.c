#include "display_ingress_simulated_service_private.h"
#include "../targets/k230d-linux/display_ingress_video_backend.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One State transaction plus the adapter's one flip. No pixel queue. */
static struct {
    MeisoIngressVideo *video;
    MeisoStateReplica *replica;
    ServiceStateArena arena;
    uint32_t *pixels;
    MeisoDisplayOverlayPlan plan;
    MeisoDisplayOverlayEvidence evidence;
    uint64_t desired_version, last_generation, last_submit_ns, flip_ns;
    uint64_t actual_version, epoch, evidence_sequence, accepted, presented;
    bool pending, drawn, submitted, status_queued;
} hud;

static uint64_t get64(const uint8_t *b)
{ uint64_t v=0; for(unsigned i=0;i<8;++i) v=(v<<8)|b[i]; return v; }
static void put64(uint8_t *b, uint64_t v)
{ for(unsigned i=0;i<8;++i) { b[7-i]=(uint8_t)v; v>>=8; } }
static bool route_is(Service *s, MeisoRuntimeRouteKey got, unsigned i)
{
    MeisoRuntimeRouteKey r=service_route_key(s,(MeisoDisplayIngressProfileRoute)i);
    return got.catalog_generation==r.catalog_generation && got.endpoint_incarnation==r.endpoint_incarnation &&
        got.facet==r.facet && meiso_endpoint_id_equal(got.endpoint_id,r.endpoint_id) &&
        meiso_resource_id_equal(got.resource_id,r.resource_id);
}

/* Built-in bitmap glyphs, intentionally limited by the selected capability. */
static const uint8_t font[36][7]={
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}
};
static unsigned glyph(char c, unsigned row)
{
    if(c>='A' && c<='Z') return font[c-'A'][row];
    if(c>='0' && c<='9') return font[26+c-'0'][row];
    if(c=='-') return row==3?31:0;
    if(c=='.') return row==6?4:0;
    if(c==':') return row==2 || row==5?4:0;
    if(c=='/') return 1U<<(row*4/6);
    return 0;
}
static void draw(void)
{
    const MeisoDisplayOverlayPlan *p=&hud.plan;
    if(!p->visible) { memset(hud.pixels,0,16*16*sizeof(uint32_t)); return; }
    for(unsigned y=0;y<p->height;++y) for(unsigned x=0;x<p->width;++x)
        hud.pixels[y*p->width+x]=(x<p->border_width || y<p->border_width ||
            x>=p->width-p->border_width || y>=p->height-p->border_width)?p->border_argb:p->fill_argb;
    for(unsigned g=0;g<p->text_size;++g) for(unsigned y=0;y<7;++y) for(unsigned x=0;x<5;++x)
        if(glyph(p->text[g],y)&(1U<<(4-x)))
            for(unsigned yy=0;yy<p->text_scale;++yy) for(unsigned xx=0;xx<p->text_scale;++xx)
                hud.pixels[(p->border_width+y*p->text_scale+yy)*p->width+
                    p->border_width+(g*6+x)*p->text_scale+xx]=p->text_argb;
}

bool service_overlay_initialize(Service *s, MeisoIngressVideo *video)
{
    memset(&hud,0,sizeof(hud)); hud.video=video;
    if(!s->overlay_enabled) return true;
    const MeisoResourceDescriptor *r=&s->routes[MEISO_DISPLAY_OVERLAY_ROUTE_PLAN].route.resource;
    size_t bytes=meiso_state_replica_memory_required(r);
    if(!bytes || bytes>sizeof(hud.arena.bytes) ||
        meiso_state_replica_init(hud.arena.bytes,bytes,r,&hud.replica)!=MEISO_STATE_INIT_OK ||
        meiso_state_replica_begin_owner(hud.replica,s->host_owner)!=MEISO_STATE_OWNER_STARTED)
        return service_fail(s,"hud","state-init");
    hud.pixels=calloc(MEISO_DISPLAY_OVERLAY_MAX_PIXELS,sizeof(uint32_t));
    return hud.pixels!=NULL || service_fail(s,"hud","pixel-budget");
}
bool service_overlay_consume(Service *s, const MeisoRuntimeEvent *event)
{
    if(!s->overlay_enabled) return service_consume_optional_accept(s,event);
    if(event->type==MEISO_RUNTIME_EVENT_STATE_ACCEPT && route_is(s,event->route,MEISO_DISPLAY_OVERLAY_ROUTE_STATUS))
        return (event->state_accept_generation.value==9 && event->state_accept_version &&
            event->state_accept_version<=hud.actual_version && meiso_owner_scope_equal(event->state_accept_owner,s->edge_owner)) ||
            service_fail(s,"hud","status-ack");
    if(event->type!=MEISO_RUNTIME_EVENT_STATE_OFFER || !route_is(s,event->route,MEISO_DISPLAY_OVERLAY_ROUTE_PLAN))
        return service_consume_optional_accept(s,event);
    const MeisoStateOffer *offer=&event->state_offer;
    MeisoRuntimeDesiredState desired; MeisoDisplayOverlayPlan plan;
    if(!meiso_owner_scope_equal(offer->state.owner,s->host_owner) || offer->generation.value!=9 ||
        !meiso_runtime_desired_state_decode(offer->state.payload,&desired) || !desired.enabled ||
        !meiso_display_overlay_plan_decode(desired.profile,&plan) ||
        !meiso_display_overlay_plan_supported(&s->overlay_capability,&plan,s->applied_desired_version)) {
        (void)meiso_runtime_commit_state_receive(s->runtime,event->route,offer,MEISO_STATE_RECEIVE_INVALID);
        return service_fail(s,"hud","invalid-plan");
    }
    uint8_t digest[32];
    const uint64_t version=offer->state.version;
    /* Runtime byte views expire at commit_state_receive. Preserve the identity
     * while the borrowed Desired suffix is still valid. */
    if(!meiso_display_ingress_host_fixture_input_sha256(desired.profile,digest))
        return service_fail(s,"hud","plan-digest");
    const MeisoStateReceiveStatus received=meiso_state_replica_receive(hud.replica,offer->state.owner,
        offer->state.version,offer->state.payload);
    if(meiso_runtime_commit_state_receive(s->runtime,event->route,offer,received)!=MEISO_RUNTIME_OK)
        return service_fail(s,"hud","plan-commit");
    if(received==MEISO_STATE_RECEIVE_DUPLICATE || received==MEISO_STATE_RECEIVE_STALE) return true;
    if(received!=MEISO_STATE_RECEIVE_NEW || hud.pending || plan.generation<=hud.last_generation)
        return service_fail(s,"hud","plan-order");
    hud.plan=plan; hud.desired_version=version; hud.last_generation=plan.generation;
    hud.evidence=(MeisoDisplayOverlayEvidence){.plan_generation=plan.generation,.desired_version=version};
    memcpy(hud.evidence.plan_sha256,digest,sizeof(digest));
    hud.pending=true; hud.drawn=false; hud.submitted=false; hud.status_queued=false; hud.flip_ns=0; ++hud.accepted;
    return true;
}
bool service_overlay_poll(Service *s)
{
    if(!s->overlay_enabled || !hud.pending) return true;
    MeisoIngressVideoPresentation flip;
    MeisoIngressVideoPollResult polled=meiso_ingress_video_overlay_poll(hud.video,&flip);
    if(polled==MEISO_INGRESS_VIDEO_POLL_ERROR) return service_fail(s,"hud","poll");
    if(polled==MEISO_INGRESS_VIDEO_PRESENTED) {
        if(!hud.submitted || flip.sequence!=hud.desired_version || !flip.page_flip_monotonic_ns)
            return service_fail(s,"hud","flip-identity");
        hud.flip_ns=flip.page_flip_monotonic_ns; ++hud.presented;
    }
    if(!hud.submitted) {
        uint64_t now=service_now_ns();
        if(now-hud.last_submit_ns<UINT64_C(1000000000)/s->overlay_capability.max_updates_per_second) return true;
        if(!hud.drawn) { draw(); hud.drawn=true; }
        const MeisoDisplayOverlayPlan *p=&hud.plan;
        MeisoIngressVideoSubmitResult r=meiso_ingress_video_overlay_submit(hud.video,hud.desired_version,
            p->visible?p->x:0,p->visible?p->y:0,p->visible?p->width:16,p->visible?p->height:16,
            hud.pixels,p->visible?(size_t)p->width*p->height*4:16*16*4);
        if(r==MEISO_INGRESS_VIDEO_WOULD_BLOCK) return true;
        if(r!=MEISO_INGRESS_VIDEO_ACCEPTED) return service_fail(s,"hud","submit");
        hud.submitted=true; hud.last_submit_ns=now;
    }
    if(!hud.flip_ns || !service_session_tx_idle(s)) return true;
    if(!hud.status_queued) {
        uint8_t profile[8],wire[MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE+8]; MeisoByteView bytes;
        put64(profile,hud.plan.generation);
        MeisoRuntimeActualState actual={0};
        actual.observed_desired=(MeisoRuntimeDesiredReference){true,s->host_owner,hud.desired_version};
        actual.applied_desired=actual.observed_desired; actual.apply_result=MEISO_RUNTIME_APPLY_APPLIED;
        actual.availability=MEISO_RUNTIME_AVAILABILITY_PRESENT; actual.effective_enabled=true;
        actual.production_status=MEISO_RUNTIME_PRODUCTION_RUNNING; actual.production_epoch=hud.epoch+1;
        actual.profile=(MeisoByteView){profile,8};
        if(!meiso_runtime_actual_state_encode(&actual,(MeisoMutableBuffer){wire,sizeof(wire)},&bytes))
            return service_fail(s,"hud","status-codec");
        MeisoStateOffer offer={{9},{s->edge_owner,hud.actual_version+1,bytes}};
        if(meiso_runtime_send_state_offer(s->runtime,service_route_key(s,MEISO_DISPLAY_OVERLAY_ROUTE_STATUS),&offer)!=MEISO_RUNTIME_OK)
            return service_fail(s,"hud","status-send");
        ++hud.epoch; ++hud.actual_version; hud.status_queued=true; return true;
    }
    uint8_t wire[56];
    if(!meiso_display_overlay_evidence_encode(&hud.evidence,(MeisoMutableBuffer){wire,sizeof(wire)}))
        return service_fail(s,"hud","evidence-codec");
    MeisoStreamSample sample={0}; sample.key=(MeisoStreamKey){s->edge_owner,1,++hud.evidence_sequence};
    sample.source_time=(MeisoTimePoint){SERVICE_EVIDENCE_CLOCK_EPOCH,hud.flip_ns}; sample.payload=(MeisoByteView){wire,56};
    if(meiso_runtime_send_stream_sample(s->runtime,service_route_key(s,MEISO_DISPLAY_OVERLAY_ROUTE_EVIDENCE),hud.epoch,&sample)!=MEISO_RUNTIME_OK)
        return service_fail(s,"hud","evidence-send");
    fprintf(stderr,"HUD_PRESENTED version=%"PRIu64" plan=%"PRIu64" epoch=%"PRIu64" flip_ns=%"PRIu64" visible=%u\n",
        hud.desired_version,get64(wire+8),hud.epoch,hud.flip_ns,hud.plan.visible);
    hud.pending=false; return true;
}
void service_overlay_finish(void)
{
    if(hud.pixels) fprintf(stderr,"HUD_RESULT accepted=%"PRIu64" presented=%"PRIu64" pending=%d cpu_bytes=262144\n",
        hud.accepted,hud.presented,hud.pending);
    free(hud.pixels); hud.pixels=NULL;
}
