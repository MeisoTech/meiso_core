#define _POSIX_C_SOURCE 200809L
#include <meiso/integrations/display_camera_channel.h>
#include <meiso/profiles/display_ingress/application.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
typedef union {max_align_t align;uint8_t bytes[65536];} Arena;
typedef struct {Arena sm,rm;MeisoSession *s;MeisoRuntime *r;MeisoDisplayCameraChannel *c;} Peer;
static Peer h,e;
static bool application,lease;
static unsigned profile_version(void) {return lease?11:9;}
static void transfer(Peer *from,Peer *to)
{
    MeisoByteView bytes;uint8_t copy[5000];MeisoSessionRuntimeMessage message;
    if(!meiso_session_next_outbound(from->s,&bytes)) return;
    CHECK(bytes.size<=sizeof(copy));memcpy(copy,bytes.data,bytes.size);
    CHECK(meiso_session_mark_outbound_sent(from->s)==MEISO_SESSION_OUTBOUND_SENT);
    MeisoSessionReceiveStatus status=meiso_session_receive(to->s,(MeisoByteView){copy,bytes.size},&message);
    if(status==MEISO_SESSION_RECEIVE_HANDSHAKE_PROGRESS) return;
    CHECK(status==MEISO_SESSION_RECEIVE_RUNTIME);
    MeisoRuntimeEvent event;CHECK(meiso_runtime_receive(to->r,&message,&event)==MEISO_RUNTIME_OK);
    if(event.type==MEISO_RUNTIME_EVENT_CATALOG_OFFERED) CHECK(meiso_runtime_accept_catalog(to->r)==MEISO_RUNTIME_OK);
    else if(event.type!=MEISO_RUNTIME_EVENT_CATALOG_ACCEPTED) CHECK(meiso_display_camera_channel_consume(to->c,&event)==1);
}
static void init(Peer *p,bool host)
{
    MeisoSessionConfig cfg={0};cfg.local_role=host?MEISO_SESSION_ROLE_HOST:MEISO_SESSION_ROLE_EDGE;
    cfg.local_peer_id=(MeisoPeerId){host?1:2,3};cfg.expected_peer_id=(MeisoPeerId){host?2:1,3};
    cfg.local_incarnation=host?4:5;cfg.local_nonce=(MeisoSessionNonce){host?6:7,8};cfg.protocol=(MeisoProtocolVersion){1,0};
    CHECK(application?meiso_display_application_configure_session_for_version(&cfg,profile_version()):meiso_display_camera_configure_session(&cfg));
    CHECK(meiso_session_init(p->sm.bytes,sizeof(p->sm.bytes),&cfg,&p->s)==MEISO_SESSION_INIT_OK);
    CHECK(meiso_session_start(p->s)==MEISO_SESSION_START_STARTED);
}
static void runtime(Peer *p)
{
    MeisoRuntimeV4Config cfg;CHECK(application?meiso_display_application_configure_runtime_for_version(&cfg,p->s,profile_version()):meiso_display_camera_configure_runtime(&cfg,p->s));
    CHECK(meiso_runtime_v4_init(p->rm.bytes,sizeof(p->rm.bytes),&cfg,&p->r)==MEISO_RUNTIME_INIT_OK);
}
static void tick(void)
{
    transfer(&h,&e);transfer(&e,&h);
    CHECK(meiso_display_camera_channel_poll(h.c));CHECK(meiso_display_camera_channel_poll(e.c));
    (void)poll(NULL,0,1);
}
int main(int argc,char **argv)
{
    const bool nv12=argc>=2 && strcmp(argv[1],"--nv12")==0;
    const bool jpeg=argc>=2 && strcmp(argv[1],"--jpeg")==0;
    lease=argc==3 && strcmp(argv[2],"--lease")==0;
    application=lease || (argc==3 && strcmp(argv[2],"--application")==0);
    CHECK(argc==1 || (argc==2 && (nv12 || jpeg)) || (application && (nv12 || jpeg || strcmp(argv[1],"--rgba")==0)));
    const size_t pixel_bytes=jpeg?10013U:nv12?115200U:307200U;
    init(&h,true);init(&e,false);transfer(&h,&e);transfer(&e,&h);transfer(&h,&e);transfer(&e,&h);
    runtime(&h);runtime(&e);
    MeisoDisplayIngressCatalogConfig v={1,{1280,720,30000,131072,1024,1,MEISO_STREAM_DEADLINE_UNSUPPORTED,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    MeisoCompositionMediaCapability c={.source=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,.pixel_formats=nv12?MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12:MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,.codecs=1,
        .min_width=320,.min_height=240,.max_width=320,.max_height=240,.max_fps_millihz=2000,
        .max_access_unit_bytes=jpeg?30720U:(uint32_t)pixel_bytes,.max_fragment_payload_bytes=4096};
    if(jpeg) {c.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_JPEG;c.max_fps_millihz=500;}
    uint8_t caps[148];MeisoRuntimeEndpointDescriptor ep[3];MeisoRuntimeV4RouteDescriptor rt[10];
    if(application){
        const MeisoDisplayApplicationCatalogConfig all={.video=v,.output_width=1920,.output_height=1080,.video_layout=1,
            .overlay_enabled=true,.camera_enabled=true,.overlay={1920,1080,65536,48,10,16,16,2,1,3,1},.camera=c,
            .scene={1920,1080,1152000,48,10,16,16,8,1,16,1}};
        CHECK(meiso_display_application_build_catalog_for_version(&all,profile_version(),(MeisoMutableBuffer){caps,sizeof(caps)},ep,3,rt,10));
    }else CHECK(meiso_display_camera_build_catalog(&v,&c,(MeisoMutableBuffer){caps,sizeof(caps)},ep,3,rt,10));
    CHECK(meiso_runtime_v4_offer_catalog(e.r,1,ep,application?3:2,rt,application?10:7)==MEISO_RUNTIME_OK);transfer(&e,&h);transfer(&h,&e);
    int listener=socket(AF_INET,SOCK_STREAM,0);CHECK(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(UINT32_C(0x7f000001))}};
    CHECK(bind(listener,(struct sockaddr*)&address,sizeof(address))==0 && listen(listener,1)==0);
    socklen_t length=sizeof(address);CHECK(getsockname(listener,(struct sockaddr*)&address,&length)==0);
    int edge_fd=socket(AF_INET,SOCK_STREAM,0);CHECK(edge_fd>=0);
    CHECK(connect(edge_fd,(struct sockaddr*)&address,sizeof(address))==0);
    int host_fd=accept(listener,NULL,NULL);CHECK(host_fd>=0);close(listener);
    CHECK(fcntl(edge_fd,F_SETFL,O_NONBLOCK)==0 && fcntl(host_fd,F_SETFL,O_NONBLOCK)==0);
    MeisoDisplayCameraChannelConfig hc={.session=h.s,.runtime=h.r,.media_fd=host_fd,.desired={nv12?MEISO_COMPOSITION_MEDIA_PIXEL_NV12:MEISO_COMPOSITION_MEDIA_PIXEL_RGBA8888,1,320,240,2000,0,1,0}};
    if(jpeg) {
        hc.desired.codec=MEISO_COMPOSITION_MEDIA_CODEC_JPEG;hc.desired.fps_millihz=500;
        CHECK(meiso_composition_media_desired_supported(&c,&hc.desired));
        MeisoCompositionMediaCapability old=c;old.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW;
        CHECK(!meiso_composition_media_desired_supported(&old,&hc.desired));
        MeisoCompositionMediaDesired invalid=hc.desired;invalid.pixel_format=MEISO_COMPOSITION_MEDIA_PIXEL_NV12;
        CHECK(!meiso_composition_media_desired_supported(&c,&invalid));
        invalid=hc.desired;invalid.fps_millihz=501;
        CHECK(!meiso_composition_media_desired_supported(&c,&invalid));
    }
    MeisoDisplayCameraChannelConfig ec={.session=e.s,.runtime=e.r,.media_fd=edge_fd,.edge_nonce={91,92}};
    if(lease) hc.application_version=ec.application_version=11;
    h.c=meiso_display_camera_channel_open(&hc);e.c=meiso_display_camera_channel_open(&ec);CHECK(h.c && e.c);
    CHECK(!meiso_display_camera_channel_ready(h.c) && !meiso_display_camera_channel_ready(e.c));
    for(unsigned i=0;i<2000 && !(meiso_display_camera_channel_ready(h.c) && meiso_display_camera_channel_ready(e.c));++i) tick();
    CHECK(meiso_display_camera_channel_ready(h.c) && meiso_display_camera_channel_ready(e.c));
    CHECK(!meiso_display_camera_channel_peer_eof(h.c));
    uint8_t pixels[307200];for(unsigned i=0;i<pixel_bytes;++i)pixels[i]=(uint8_t)i;
    if(jpeg) {
        /* Framing transports opaque JPEG bytes; the consumer validates the image. */
        CHECK(!meiso_display_camera_channel_submit(e.c,1,(MeisoTimePoint){9,100},(MeisoByteView){pixels,30721}));
        CHECK(!meiso_display_camera_channel_submit(e.c,1,(MeisoTimePoint){9,100},(MeisoByteView){pixels,0}));
    }
    CHECK(meiso_display_camera_channel_submit(e.c,1,(MeisoTimePoint){9,100},(MeisoByteView){pixels,pixel_bytes}));
    CHECK(!meiso_display_camera_channel_submit(e.c,1,(MeisoTimePoint){9,100},(MeisoByteView){pixels,pixel_bytes}));
    MeisoCompositionMediaLatestView frame;bool found=false;
    for(unsigned i=0;i<2000 && !found;++i) {tick();found=meiso_display_camera_channel_latest(h.c,&frame);}
    CHECK(found && frame.unit.frame_sequence==1 && frame.unit.source_time.clock_epoch==9 && frame.unit.source_time.monotonic_ns==100);
    CHECK(frame.unit.pixel_format==hc.desired.pixel_format);
    CHECK(frame.unit.codec==hc.desired.codec);
    CHECK(frame.unit.access_unit.size==pixel_bytes && !memcmp(frame.unit.access_unit.data,pixels,pixel_bytes));
    MeisoCompositionMediaFrameRef wrong=frame.ref;wrong.frame_sequence=2;
    CHECK(!meiso_display_camera_channel_release(h.c,&wrong));CHECK(meiso_display_camera_channel_release(h.c,&frame.ref));
    CHECK(!meiso_display_camera_channel_latest(h.c,&frame));
    const size_t second_bytes=jpeg?30720U:pixel_bytes;
    if(jpeg) memset(pixels,37,second_bytes);
    CHECK(meiso_display_camera_channel_submit(e.c,3,(MeisoTimePoint){9,300},(MeisoByteView){pixels,second_bytes}));
    found=false;for(unsigned i=0;i<2000 && !found;++i) {tick();found=meiso_display_camera_channel_latest(h.c,&frame);}
    CHECK(found && frame.unit.frame_sequence==3 && frame.unit.producer_dropped_frames==1);
    CHECK(frame.unit.access_unit.size==second_bytes && !memcmp(frame.unit.access_unit.data,pixels,second_bytes));
    if(lease) {
        CHECK(meiso_display_camera_channel_release(h.c,&frame.ref));
        CHECK(meiso_display_camera_channel_submit(e.c,4,(MeisoTimePoint){9,400},(MeisoByteView){pixels,second_bytes}));
        CHECK(meiso_display_camera_channel_stop(e.c,MEISO_DISPLAY_LINK_LEASE_EXPIRED));
        CHECK(meiso_display_camera_channel_stop(e.c,MEISO_DISPLAY_LINK_LEASE_EXPIRED));
        CHECK(!meiso_display_camera_channel_ready(e.c));
        CHECK(!meiso_display_camera_channel_submit(e.c,5,(MeisoTimePoint){9,500},(MeisoByteView){pixels,second_bytes}));
        for(unsigned i=0;i<10 && !meiso_display_camera_channel_stopped(h.c);++i) tick();
        CHECK(meiso_display_camera_channel_stopped(h.c) && !meiso_display_camera_channel_ready(h.c));
        CHECK(!meiso_display_camera_channel_latest(h.c,&frame));
        MeisoSessionSnapshot s;
        CHECK(meiso_session_snapshot(h.s,&s) && s.state==MEISO_SESSION_STATE_READY);
        CHECK(meiso_session_snapshot(e.s,&s) && s.state==MEISO_SESSION_STATE_READY);
        meiso_display_camera_channel_close(e.c);meiso_display_camera_channel_close(h.c);close(edge_fd);close(host_fd);
        puts("v11 camera: lease stop disposes pending media, truthful STOPPED, Session retained");return 0;
    }
    meiso_display_camera_channel_close(e.c);close(edge_fd);
    bool lost=false;for(unsigned i=0;i<100 && !lost;++i) {lost=!meiso_display_camera_channel_poll(h.c);(void)poll(NULL,0,1);}
    CHECK(lost && !meiso_display_camera_channel_ready(h.c) && !meiso_display_camera_channel_latest(h.c,&frame));
    CHECK(meiso_display_camera_channel_peer_eof(h.c));
    meiso_display_camera_channel_close(h.c);close(host_fd);
    puts("camera control, TCP bytes, readiness, source identity, stale release and close passed");return 0;
}
