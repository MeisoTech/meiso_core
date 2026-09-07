#include <meiso/profiles/display_ingress/overlay.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)

int main(void)
{
    MeisoDisplayOverlayCapability cap={1920,1080,65536,48,10,16,16,2,1}, decoded_cap;
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
    memcpy(wire,canonical,sizeof(wire)); wire[57]=1;
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

    MeisoDisplayIngressCatalogConfig video={1,{1280,720,60000,131072,1200,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_DRM_KMS_PAGE_FLIP,MEISO_STREAM_DEADLINE_UNSUPPORTED,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    uint8_t capabilities[64]; MeisoRuntimeEndpointDescriptor endpoints[2]; MeisoRuntimeV4RouteDescriptor routes[7];
    MeisoDisplayIngressCapability decoded_video;
    CHECK(meiso_display_overlay_build_catalog(&video,&cap,(MeisoMutableBuffer){capabilities,64},endpoints,2,routes,7));
    CHECK(meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    CHECK(decoded_cap.output_width==1920 && decoded_cap.max_updates_per_second==10);
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,1,routes,4,&decoded_video,&decoded_cap));
    CHECK(meiso_display_ingress_validate_catalog(endpoints,2,routes,7,&decoded_video)==MEISO_DISPLAY_INGRESS_PROFILE_CATALOG_MISMATCH);
    CHECK(meiso_display_ingress_validate_catalog(endpoints,1,routes,4,&decoded_video)==MEISO_DISPLAY_INGRESS_PROFILE_OK);
    routes[4].route.resource.owner=MEISO_OWNER_ROLE_EDGE;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    routes[4].route.resource.owner=MEISO_OWNER_ROLE_HOST; routes[6].delivery=MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    routes[6].delivery=MEISO_RUNTIME_DELIVERY_INLINE; capabilities[63]=1;
    CHECK(!meiso_display_overlay_validate_catalog(endpoints,2,routes,7,&decoded_video,&decoded_cap));
    CHECK(!meiso_display_overlay_build_catalog(&video,&cap,(MeisoMutableBuffer){capabilities,64},endpoints,2,routes,6));

    MeisoDisplayOverlayEvidence evidence={501,17,{1,2,3}}, got;
    CHECK(meiso_display_overlay_evidence_encode(&evidence,(MeisoMutableBuffer){wire,sizeof(wire)}));
    CHECK(meiso_display_overlay_evidence_decode((MeisoByteView){wire,56},&got));
    CHECK(got.plan_generation==501 && got.desired_version==17 && !memcmp(got.plan_sha256,evidence.plan_sha256,32));
    wire[0]=1; CHECK(!meiso_display_overlay_evidence_decode((MeisoByteView){wire,56},&got));
    wire[0]=2; wire[3]=1; CHECK(!meiso_display_overlay_evidence_decode((MeisoByteView){wire,56},&got));
    puts("Display overlay catalog/plan/physical-evidence vectors passed"); return 0;
}
