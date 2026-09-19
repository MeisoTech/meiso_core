#include <meiso/integrations/display_link_channel.h>
#include <meiso/profiles/display_ingress/application.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef union { max_align_t align; uint8_t bytes[65536]; } Arena;
typedef struct { Arena sm,rm; MeisoSession *s; MeisoRuntime *r; MeisoDisplayLinkChannel *c; unsigned offers,accepts; } Peer;
static Peer h,e;
static void transfer(Peer *from, Peer *to)
{
    MeisoByteView bytes; uint8_t copy[5000]; MeisoSessionRuntimeMessage message;
    if (!meiso_session_next_outbound(from->s,&bytes)) return;
    CHECK(bytes.size<=sizeof(copy)); memcpy(copy,bytes.data,bytes.size);
    CHECK(meiso_session_mark_outbound_sent(from->s)==MEISO_SESSION_OUTBOUND_SENT);
    const MeisoSessionReceiveStatus status=meiso_session_receive(to->s,(MeisoByteView){copy,bytes.size},&message);
    if (status==MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS) return;
    CHECK(status==MEISO_SESSION_RECEIVE_RUNTIME);
    MeisoRuntimeEvent event; CHECK(meiso_runtime_receive(to->r,&message,&event)==MEISO_RUNTIME_OK);
    if (event.type==MEISO_RUNTIME_EVENT_CATALOG_OFFERED) CHECK(meiso_runtime_accept_catalog(to->r)==MEISO_RUNTIME_OK);
    else if (event.type!=MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED) {
        to->offers+=event.type==MEISO_RUNTIME_EVENT_STATE_OFFER;
        to->accepts+=event.type==MEISO_RUNTIME_EVENT_STATE_ACCEPT;
        CHECK(meiso_display_link_channel_consume(to->c,&event)==1);
        /* Optional transport acceptance deliberately precedes LINK_ACT, to
         * prove the Host does not confuse the two. */
        if (event.type==MEISO_RUNTIME_EVENT_STATE_OFFER)
            CHECK(meiso_runtime_send_state_accept(to->r,event.route,event.state_offer.generation,
                event.state_offer.state.owner,event.state_offer.state.version)==MEISO_RUNTIME_OK);
    }
}
static void init(Peer *p, bool host)
{
    MeisoSessionConfig cfg={0};cfg.local_role=host?MEISO_SESSION_ROLE_HOST:MEISO_SESSION_ROLE_EDGE;
    cfg.local_peer_id=(MeisoPeerId){host?1:2,3};cfg.expected_peer_id=(MeisoPeerId){host?2:1,3};
    cfg.local_incarnation=host?4:5;cfg.local_nonce=(MeisoSessionNonce){host?6:7,8};cfg.protocol=(MeisoProtocolVersion){1,0};
    CHECK(meiso_display_application_configure_session_for_version(&cfg,11));
    CHECK(meiso_session_memory_required(&cfg)<=sizeof(p->sm.bytes));
    CHECK(meiso_session_init(p->sm.bytes,sizeof(p->sm.bytes),&cfg,&p->s)==MEISO_SESSION_INIT_OK);
    CHECK(meiso_session_start(p->s)==MEISO_SESSION_START_STARTED);
}
static void runtime(Peer *p)
{
    MeisoRuntimeV4Config cfg; CHECK(meiso_display_application_configure_runtime_for_version(&cfg,p->s,11));
    const size_t bytes=meiso_runtime_v4_memory_required(&cfg);
    CHECK(bytes && bytes<=sizeof(p->rm.bytes));
    printf("v11 runtime arena required=%zu bound=%zu\n",bytes,sizeof(p->rm.bytes));
    CHECK(meiso_runtime_v4_init(p->rm.bytes,sizeof(p->rm.bytes),&cfg,&p->r)==MEISO_RUNTIME_INIT_OK);
}
int main(void)
{
    init(&h,true);init(&e,false);transfer(&h,&e);transfer(&e,&h);transfer(&h,&e);transfer(&e,&h);
    runtime(&h);runtime(&e);
    MeisoDisplayApplicationCatalogConfig config={0};
    config.video=(MeisoDisplayIngressCatalogConfig){1,{1280,720,30000,131072,1024,1,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    config.output_width=1920;config.output_height=1080;config.link_enabled=true;
    config.link=(MeisoDisplayLinkCapability){1,{1000,0,0},{1000,0,0},1280*720,1920*1080};
    uint8_t caps[188];MeisoRuntimeEndpointDescriptor ep[4];MeisoRuntimeV4RouteDescriptor rt[12];
    CHECK(meiso_display_application_build_catalog_for_version(&config,11,(MeisoMutableBuffer){caps,188},ep,4,rt,12));
    CHECK(meiso_runtime_v4_offer_catalog(e.r,1,ep,2,rt,6)==MEISO_RUNTIME_OK);transfer(&e,&h);transfer(&h,&e);
    const MeisoDisplayLinkDesired desired={0,1000,1000,1000,5,{3000,3000,4000},750};
    e.c=meiso_display_link_channel_open(e.s,e.r,NULL);h.c=meiso_display_link_channel_open(h.s,h.r,&desired);
    CHECK(h.c && e.c);
    MeisoDisplayLinkDesired effective;
    CHECK(!meiso_display_link_channel_effective(h.c,&effective));
    CHECK(meiso_display_link_channel_poll(h.c,100));transfer(&h,&e);transfer(&e,&h);
    CHECK(e.offers==1 && h.accepts==1);
    CHECK(meiso_display_link_channel_effective(e.c,&effective)==1);
    CHECK(!meiso_display_link_channel_effective(h.c,&effective)); /* ACK != liveness */
    CHECK(meiso_display_link_channel_poll(h.c,2100));transfer(&h,&e);
    CHECK(e.offers==1); /* one outstanding despite two elapsed intervals */
    CHECK(meiso_display_link_channel_poll(e.c,2100));transfer(&e,&h);transfer(&h,&e);
    CHECK(meiso_display_link_channel_effective(h.c,&effective)==1 && h.offers==1 && e.accepts==1);
    CHECK(meiso_display_link_channel_poll(h.c,2200));transfer(&h,&e);transfer(&e,&h);
    CHECK(e.offers==2 && meiso_display_link_channel_effective(e.c,&effective)==2);
    CHECK(meiso_display_link_channel_poll(e.c,2200));transfer(&e,&h);transfer(&h,&e);
    CHECK(meiso_display_link_channel_effective(h.c,&effective)==2);
    CHECK(meiso_display_link_channel_poll(h.c,3199));transfer(&h,&e);CHECK(e.offers==2);
    CHECK(meiso_display_link_channel_poll(h.c,3200));transfer(&h,&e);transfer(&e,&h);CHECK(e.offers==3);
    CHECK(meiso_display_link_channel_poll(h.c,8199));
    MeisoSessionSnapshot state;CHECK(meiso_session_snapshot(h.s,&state) && state.state==MEISO_SESSION_STATE_READY);
    CHECK(!meiso_display_link_channel_poll(h.c,8200));
    CHECK(meiso_session_snapshot(h.s,&state) && state.state==MEISO_SESSION_STATE_FAILED);
    CHECK(!meiso_display_link_channel_poll(h.c,9000)); /* sticky */
    CHECK(meiso_display_link_channel_poll(e.c,9000000));
    CHECK(meiso_session_snapshot(e.s,&state) && state.state==MEISO_SESSION_STATE_READY); /* Edge leases only */
    meiso_display_link_channel_close(h.c);meiso_display_link_channel_close(e.c);
    puts("v11 real Session/Runtime heartbeat: ACK isolation, one outstanding, sticky Host loss, live Edge passed");
    return 0;
}
