#include <meiso/profiles/display_ingress/application.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void)
{
    MeisoDisplayApplicationCatalogConfig c={0}, decoded;
    c.video=(MeisoDisplayIngressCatalogConfig){1,{1280,720,30000,131072,1024,
        MEISO_DISPLAY_INGRESS_EVIDENCE_MASK_SIMULATED,MEISO_STREAM_DEADLINE_UNSUPPORTED,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    c.output_width=1920;c.output_height=1080;c.video_layout=1;
    c.overlay=(MeisoDisplayOverlayCapability){1920,1080,65536,48,10,16,16,2,1,3,1};
    c.camera=(MeisoCompositionMediaCapability){.source=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats=MEISO_COMPOSITION_MEDIA_PIXEL_MASK_RGBA8888,.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width=320,.min_height=240,.max_width=320,.max_height=240,.max_fps_millihz=5000,
        .max_access_unit_bytes=307200,.max_fragment_payload_bytes=4096};
    uint8_t bytes[148]; MeisoRuntimeEndpointDescriptor ep[3];MeisoRuntimeV4RouteDescriptor rt[10];
    for(unsigned mask=0;mask<4;++mask){
        c.overlay_enabled=(mask&1)!=0;c.camera_enabled=(mask&2)!=0;
        size_t ec=1+c.overlay_enabled+c.camera_enabled,rc=4+3*(ec-1);
        CHECK(meiso_display_application_build_catalog(&c,(MeisoMutableBuffer){bytes,sizeof(bytes)},ep,3,rt,10));
        CHECK(meiso_display_application_validate_catalog(ep,ec,rt,rc,&decoded));
        CHECK(decoded.overlay_enabled==c.overlay_enabled && decoded.camera_enabled==c.camera_enabled &&
            decoded.output_width==1920 && decoded.video_layout==1);
        CHECK(!meiso_display_application_validate_catalog(ep,ec,rt,rc-1,&decoded));
        bytes[47]=1;CHECK(!meiso_display_application_validate_catalog(ep,ec,rt,rc,&decoded));bytes[47]=0;
        rt[rc-1].route.resource.owner=MEISO_OWNER_ROLE_HOST;
        CHECK(!meiso_display_application_validate_catalog(ep,ec,rt,rc,&decoded));
    }
    c.overlay.output_width=1280;
    CHECK(!meiso_display_application_build_catalog(&c,(MeisoMutableBuffer){bytes,sizeof(bytes)},ep,3,rt,10));
    c.overlay.output_width=1920;c.output_width=1279;
    CHECK(!meiso_display_application_build_catalog(&c,(MeisoMutableBuffer){bytes,sizeof(bytes)},ep,3,rt,10));
    c.output_width=1920;c.video.capability.evidence_classes=3;
    CHECK(!meiso_display_application_build_catalog(&c,(MeisoMutableBuffer){bytes,sizeof(bytes)},ep,3,rt,10));
    MeisoDisplayOverlayEvidence proof={1,2,{0}}, got;
    CHECK(meiso_display_overlay_evidence_encode_for_class(&proof,1,(MeisoMutableBuffer){bytes,56}));
    CHECK(meiso_display_overlay_evidence_decode_for_class((MeisoByteView){bytes,56},1,&got));
    CHECK(!meiso_display_overlay_evidence_decode_for_class((MeisoByteView){bytes,56},2,&got));
    CHECK(!meiso_display_overlay_evidence_decode((MeisoByteView){bytes,56},&got));
    CHECK(!meiso_display_overlay_evidence_encode_for_class(&proof,3,(MeisoMutableBuffer){bytes,56}));
    c.video.capability.evidence_classes=1;c.overlay_enabled=true;
    c.scene=(MeisoDisplaySceneCapability){1920,1080,1152000,48,10,16,16,8,1,16,1};
    for(unsigned camera=0;camera<2;++camera) {
        c.camera_enabled=camera!=0;const size_t ec=2+camera,rc=7+3*camera;
        CHECK(meiso_display_application_build_catalog_for_version(&c,10,(MeisoMutableBuffer){bytes,148},ep,3,rt,10));
        CHECK(meiso_display_application_validate_catalog_for_version(ep,ec,rt,rc,10,&decoded));
        CHECK(decoded.scene.max_pixels==1152000 && ep[1].capability_schema.version==4 &&
            rt[4].route.resource.max_payload_size==MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE+1440);
        CHECK(!meiso_display_application_validate_catalog(ep,ec,rt,rc,&decoded));
        rt[4].route.resource.schema.version=3;
        CHECK(!meiso_display_application_validate_catalog_for_version(ep,ec,rt,rc,10,&decoded));
        CHECK(meiso_display_application_build_catalog(&c,(MeisoMutableBuffer){bytes,148},ep,3,rt,10));
        CHECK(!meiso_display_application_validate_catalog_for_version(ep,ec,rt,rc,10,&decoded));
    }
    CHECK(!meiso_display_application_build_catalog_for_version(&c,12,(MeisoMutableBuffer){bytes,148},ep,3,rt,10));
    puts("application combinations, geometry and evidence isolation passed");return 0;
}
