#include <meiso/profiles/display_ingress/camera.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void)
{
    MeisoDisplayIngressCatalogConfig v={1,{1280,720,30000,131072,1024,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_SIMULATED,MEISO_STREAM_DEADLINE_UNSUPPORTED,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    MeisoCompositionMediaCapability c={.source=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats=MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width=320,.min_height=240,.max_width=320,.max_height=240,.max_fps_millihz=5000,
        .max_access_unit_bytes=320*240*4,.max_fragment_payload_bytes=4096}, camera;
    MeisoDisplayIngressCapability video;
    uint8_t caps[96]; MeisoRuntimeEndpointDescriptor ep[2]; MeisoRuntimeV4RouteDescriptor rt[7];
    CHECK(meiso_display_camera_build_catalog(&v,&c,(MeisoMutableBuffer){caps,96},ep,2,rt,7));
    CHECK(meiso_display_camera_validate_catalog(ep,2,rt,7,&video,&camera));
    CHECK(camera.max_access_unit_bytes==307200 && video.frame_format==MEISO_DISPLAY_INGRESS_FRAME_H264_IDR);
    CHECK(meiso_display_ingress_validate_catalog(ep,2,rt,7,&video)!=MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(!meiso_display_camera_validate_catalog(ep,1,rt,4,&video,&camera));
    rt[6].delivery=MEISO_RUNTIME_DELIVERY_INLINE;
    CHECK(!meiso_display_camera_validate_catalog(ep,2,rt,7,&video,&camera));
    rt[6].delivery=MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA;
    rt[4].route.resource.owner=MEISO_OWNER_ROLE_EDGE;
    CHECK(!meiso_display_camera_validate_catalog(ep,2,rt,7,&video,&camera));
    memset(caps,0xa5,sizeof(caps));
    CHECK(!meiso_display_camera_build_catalog(&v,&c,(MeisoMutableBuffer){caps,95},ep,2,rt,7));
    CHECK(caps[0]==0xa5 && caps[95]==0xa5);
    c.source=MEISO_COMPOSITION_MEDIA_SOURCE_RENDER_PREVIEW;
    CHECK(!meiso_display_camera_build_catalog(&v,&c,(MeisoMutableBuffer){caps,96},ep,2,rt,7));
    MeisoSessionConfig s={0};s.local_role=MEISO_SESSION_ROLE_HOST;
    s.local_peer_id=(MeisoPeerId){11,12};s.expected_peer_id=(MeisoPeerId){21,22};
    s.local_incarnation=1;s.local_nonce=(MeisoSessionNonce){3,4};s.protocol=(MeisoProtocolVersion){1,0};
    CHECK(meiso_display_camera_configure_session(&s) && s.profile.version==8);
    CHECK(meiso_display_ingress_configure_session_for_format(&s,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR)==MEISO_DISPLAY_INGRESS_PROFILE_OK);
    CHECK(s.profile.version==2);
    puts("display/camera graph and version isolation passed");return 0;
}
