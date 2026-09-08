#define _POSIX_C_SOURCE 200809L
#include <meiso/profiles/display_ingress/overlay.h>
#include "../targets/k230d-linux/display_ingress_overlay_raster.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)

int main(void)
{
    MeisoSessionConfig session={0};
    session.local_role=MEISO_SESSION_ROLE_HOST;
    session.local_peer_id=(MeisoPeerId){11,12}; session.expected_peer_id=(MeisoPeerId){21,22};
    session.local_incarnation=31; session.local_nonce=(MeisoSessionNonce){61,62};
    session.protocol=(MeisoProtocolVersion){1,0};
    CHECK(meiso_display_overlay_configure_session(&session));
    CHECK(session.profile.version==6);
    const size_t session_bytes=meiso_session_memory_required(&session);
    CHECK(meiso_display_stereo_overlay_configure_session(&session));
    CHECK(session.profile.version==7 && meiso_session_memory_required(&session)==session_bytes);
    CHECK(!meiso_display_stereo_overlay_configure_session(NULL));
    CHECK(meiso_display_overlay_configure_session(&session) && session.profile.version==6);

    MeisoDisplayOverlayCapability cap={1920,1080,65536,48,10,16,16,2,1,3,3}, decoded_cap;
    MeisoDisplayOverlayPlan plan={0}, decoded;
    plan.generation=501; plan.video_desired_version=3; plan.canvas_width=1920; plan.canvas_height=1080;
    plan.x=100; plan.y=500; plan.width=512; plan.height=128; plan.visible=1;
    plan.border_argb=0xff00ff00; plan.fill_argb=0x60000000; plan.text_argb=0xffffffff;
    plan.border_width=4; plan.text_scale=4; plan.text_size=8; memcpy(plan.text,"HOST 001",8);
    uint8_t wire[112], canonical[112];
    CHECK(meiso_display_overlay_plan_encode(&plan,(MeisoMutableBuffer){wire,sizeof(wire)}));
    memcpy(canonical,wire,sizeof(wire));
    CHECK(wire[6]==1 && wire[7]==245 && wire[15]==3 && wire[56]==8);
    CHECK(meiso_display_overlay_plan_decode((MeisoByteView){wire,sizeof(wire)},&decoded));
    CHECK(decoded.generation==501 && decoded.x==100 && decoded.border_width==4 && decoded.text_size==8);
    CHECK(meiso_display_overlay_plan_supported(&cap,&decoded,3));
    CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,4));
    decoded.x=1800; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    decoded=plan; decoded.height=129; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    decoded=plan; decoded.width=511; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    decoded=plan; decoded.height=15; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    decoded=plan; decoded.canvas_width=1280; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    decoded=plan; decoded.text_scale=8; decoded.width=64;
    memset(wire,0xa5,sizeof(wire)); CHECK(!meiso_display_overlay_plan_encode(&decoded,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(wire[0]==0xa5 && wire[111]==0xa5);
    memcpy(wire,canonical,sizeof(wire)); wire[59]=1;
    decoded.generation=999; CHECK(!meiso_display_overlay_plan_decode((MeisoByteView){wire,sizeof(wire)},&decoded));
    CHECK(decoded.generation==999);
    memcpy(wire,canonical,sizeof(wire)); wire[111]=1;
    CHECK(!meiso_display_overlay_plan_decode((MeisoByteView){wire,sizeof(wire)},&decoded));
    memcpy(wire,canonical,sizeof(wire)); wire[64]='h';
    CHECK(!meiso_display_overlay_plan_decode((MeisoByteView){wire,sizeof(wire)},&decoded));
    memcpy(wire,canonical,sizeof(wire)); memset(wire,0,8);
    CHECK(!meiso_display_overlay_plan_decode((MeisoByteView){wire,sizeof(wire)},&decoded));
    CHECK(!meiso_display_overlay_plan_decode((MeisoByteView){canonical,111},&decoded));
    decoded=(MeisoDisplayOverlayPlan){0}; decoded.generation=900; decoded.video_desired_version=3;
    decoded.canvas_width=1920; decoded.canvas_height=1080;
    CHECK(meiso_display_overlay_plan_supported(&cap,&decoded,3));
    CHECK(meiso_display_overlay_plan_encode(&decoded,(MeisoMutableBuffer){wire,sizeof(wire)}));
    decoded.x=1; CHECK(!meiso_display_overlay_plan_encode(&decoded,(MeisoMutableBuffer){wire,sizeof(wire)}));

    decoded=plan; decoded.layout=MEISO_DISPLAY_OVERLAY_LAYOUT_SIDE_BY_SIDE;
    decoded.width=320; decoded.height=48;
    CHECK(meiso_display_overlay_plan_supported(&cap,&decoded,3));
    CHECK(meiso_display_overlay_plan_encode(&decoded,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(wire[57]==1);
    CHECK(meiso_display_overlay_plan_decode((MeisoByteView){wire,sizeof(wire)},&decoded));
    CHECK(decoded.layout==MEISO_DISPLAY_OVERLAY_LAYOUT_SIDE_BY_SIDE);
    decoded.x=641; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    decoded.x=100; decoded.height=52; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    decoded.height=48; cap.layouts=1;
    CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));
    cap.layouts=3; cap.output_width=1918; decoded.canvas_width=1918;
    CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3)); // spanning stride is odd
    cap.output_width=1919; decoded.canvas_width=1919;
    CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3)); // not two equal eyes
    cap.output_width=1920;
    decoded.layout=2;
    CHECK(!meiso_display_overlay_plan_encode(&decoded,(MeisoMutableBuffer){wire,sizeof(wire)}));

    decoded=plan; decoded.layout=1; decoded.width=384; decoded.height=48;
    decoded.text_size=15; memset(decoded.text,0,sizeof(decoded.text));
    memcpy(decoded.text,"APP MESSAGE 599",15); // largest bounded application index
    CHECK(meiso_display_overlay_plan_supported(&cap,&decoded,3));
    CHECK(meiso_display_overlay_plan_encode(&decoded,(MeisoMutableBuffer){wire,sizeof(wire)}));
    decoded.height=49; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3));

    // Check the exact pixels generated by the board renderer: two identical
    // labels at physical eye offsets, transparent gap, and bounded writes.
    static uint32_t pixels[65537];
    uint32_t w=0,h=0;
    decoded=plan; decoded.layout=1; decoded.width=320; decoded.height=48;
    memset(pixels,0xa5,sizeof(pixels));
    CHECK(meiso_k230d_overlay_raster(&cap,&decoded,pixels,65536,&w,&h));
    CHECK(w==1280 && h==48 && pixels[w*h]==0xa5a5a5a5U);
    for(unsigned y=0;y<h;++y) {
        CHECK(!memcmp(pixels+y*w,pixels+y*w+960,320*sizeof(uint32_t)));
        for(unsigned x=320;x<960;++x) CHECK(pixels[y*w+x]==0);
    }
    CHECK(pixels[0]==decoded.border_argb);
    CHECK(pixels[4*w+4]==decoded.text_argb); // H top-left glyph stroke
    CHECK(pixels[40*w+40]==decoded.fill_argb);
    pixels[0]=0x12345678; w=19; h=20;
    CHECK(!meiso_k230d_overlay_raster(&cap,&decoded,pixels,100,&w,&h));
    CHECK(pixels[0]==0x12345678 && w==19 && h==20);
    decoded=(MeisoDisplayOverlayPlan){0}; decoded.generation=999; decoded.video_desired_version=3;
    decoded.canvas_width=1920; decoded.canvas_height=1080;
    CHECK(meiso_k230d_overlay_raster(&cap,&decoded,pixels,65536,&w,&h));
    CHECK(w==16 && h==16);
    for(unsigned i=0;i<256;++i) CHECK(pixels[i]==0);

    // The same immutable State carries UTF-8 and an explicit negotiated font.
    decoded=plan; decoded.layout=1; decoded.width=320; decoded.height=48;
    decoded.font=MEISO_DISPLAY_OVERLAY_FONT_CJK16; decoded.text_scale=2;
    memset(decoded.text,0,sizeof(decoded.text));
    const char mixed[]="你好 a";
    decoded.text_size=(uint8_t)(sizeof(mixed)-1); memcpy(decoded.text,mixed,sizeof(mixed)-1);
    uint32_t tw=0,th=0;
    CHECK(meiso_display_overlay_text_extent(decoded.font,
        (MeisoByteView){(const uint8_t *)mixed,sizeof(mixed)-1},&tw,&th));
    CHECK(tw==48 && th==16);
    CHECK(meiso_display_overlay_plan_encode(&decoded,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(wire[58]==1);
    MeisoDisplayOverlayPlan mixed_decoded;
    CHECK(meiso_display_overlay_plan_decode((MeisoByteView){wire,sizeof(wire)},&mixed_decoded));
    CHECK(mixed_decoded.font==1 && !memcmp(mixed_decoded.text,mixed,sizeof(mixed)-1));
    cap.fonts=1; CHECK(!meiso_display_overlay_plan_supported(&cap,&decoded,3)); cap.fonts=3;
    CHECK(meiso_display_overlay_plan_supported(&cap,&decoded,3));
    const uint8_t malformed[][4]={{0xe4,0xbd},{0xc0,0xaf},{0xed,0xa0,0x80},
        {0xe4,0x41,0xa0},{0xf0,0x9f,0x98,0x80},{0},{0x7f},{0xc3,0xa9}};
    const size_t lengths[]={2,2,3,3,4,1,1,2};
    for(unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) {
        tw=123; th=456;
        CHECK(!meiso_display_overlay_text_extent(1,(MeisoByteView){malformed[i],lengths[i]},&tw,&th));
        CHECK(tw==123 && th==456);
    }
    CHECK(!meiso_k230d_overlay_raster(&cap,&decoded,pixels,65536,&w,&h));
    // Synthetic glyph rows exercise the exact board raster path without a font
    // dependency in the portable test. Real glyph appearance is checked on-board.
    char path[]="/tmp/meiso-overlay-font-XXXXXX";
    int fd=mkstemp(path); CHECK(fd>=0);
    CHECK(ftruncate(fd,8+65536*32)==0 && pwrite(fd,"MCJK16\r\n",8,0)==8);
    const uint8_t stroke[2]={0x80,0};
    CHECK(pwrite(fd,stroke,2,8+(off_t)0x4f60*32)==2);
    CHECK(meiso_k230d_overlay_font_open(path)); CHECK(unlink(path)==0); CHECK(close(fd)==0);
    CHECK(meiso_k230d_overlay_raster(&cap,&decoded,pixels,65536,&w,&h));
    CHECK(pixels[4*w+4]==decoded.text_argb && pixels[4*w+5]==decoded.text_argb);
    CHECK(pixels[4*w+6]==decoded.fill_argb);
    for(unsigned y=0;y<h;++y) CHECK(!memcmp(pixels+y*w,pixels+y*w+960,320*sizeof(uint32_t)));
    meiso_k230d_overlay_font_close();
    CHECK(!meiso_k230d_overlay_raster(&cap,&decoded,pixels,65536,&w,&h));

    MeisoDisplayIngressCatalogConfig video={1,{1280,720,60000,131072,1200,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP,MEISO_STREAM_DEADLINE_UNSUPPORTED,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    uint8_t capabilities[68]; MeisoRuntimeEndpointDescriptor endpoints[2]; MeisoRuntimeV4RouteDescriptor routes[7];
    MeisoDisplayIngressCapability decoded_video;
    CHECK(meiso_display_overlay_build_catalog(&video,&cap,(MeisoMutableBuffer){capabilities,sizeof(capabilities)},endpoints,2,routes,7));
    CHECK(meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    endpoints[1].capability_schema.version=1;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    endpoints[1].capability_schema.version=3; routes[4].route.resource.schema.version=1;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    routes[4].route.resource.schema.version=3;
    CHECK(decoded_cap.output_width==1920 && decoded_cap.max_updates_per_second==10);
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,1,routes,4,&decoded_video,&decoded_cap));
    CHECK(meiso_display_ingress_validate_catalog(endpoints,2,routes,7,&decoded_video)==MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH);
    CHECK(meiso_display_ingress_validate_catalog(endpoints,1,routes,4,&decoded_video)==MEISO_DISPLAY_INGRESS_PROFILE_OK);
    routes[4].route.resource.owner=MEISO_OWNER_ROLE_EDGE;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    routes[4].route.resource.owner=MEISO_OWNER_ROLE_HOST; routes[6].delivery=MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    routes[6].delivery=MEISO_RUNTIME_DELIVERY_INLINE; capabilities[63]=4;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    CHECK(!meiso_display_overlay_build_catalog(&video,&cap,(MeisoMutableBuffer){capabilities,sizeof(capabilities)},endpoints,2,routes,6));

    MeisoDisplayOverlayEvidence evidence={501,17,{1,2,3}}, got;
    CHECK(meiso_display_overlay_evidence_encode(&evidence,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(meiso_display_overlay_evidence_decode((MeisoByteView){wire,56},&got));
    CHECK(got.plan_generation==501 && got.desired_version==17 && !memcmp(got.plan_sha256,evidence.plan_sha256,32));
    wire[0]=1; CHECK(!meiso_display_overlay_evidence_decode((MeisoByteView){wire,56},&got));
    wire[0]=2; wire[3]=1; CHECK(!meiso_display_overlay_evidence_decode((MeisoByteView){wire,56},&got));
    puts("Display overlay catalog/plan/physical-evidence vectors passed"); return 0;
}
