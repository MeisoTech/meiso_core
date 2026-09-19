#include <meiso/profiles/display_ingress/application.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void)
{
    const MeisoDisplayLinkCapability cap={1,{1000,0,0},{800,0,0},1280*720,1920*1080};
    const MeisoDisplayLinkDesired desired={0,1000,800,1000,5,{3000,4000,0}};
    /* Literal network-order vectors, independent of encoder. */
    const uint8_t expected_cap[40]={0,0,0,1,0,0,3,0xe8,0,0,3,0x20,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x0e,0x10,0,0,0x1f,0xa4,0,0,0,0,0};
    const uint8_t expected_des[32]={0,0,0,0,0,0,3,0xe8,0,0,3,0x20,0,0,3,0xe8,
        0,0,0,5,0,0,0x0b,0xb8,0,0,0x0f,0xa0,0,0,0,0};
    uint8_t wire[40]; MeisoDisplayLinkCapability got_cap; MeisoDisplayLinkDesired got;
    CHECK(meiso_display_link_capability_encode(&cap,(MeisoMutableBuffer){wire,40}));
    CHECK(!memcmp(wire,expected_cap,40));
    CHECK(meiso_display_link_capability_decode((MeisoByteView){expected_cap,40},&got_cap));
    CHECK(meiso_display_link_desired_encode(&desired,(MeisoMutableBuffer){wire,40}));
    CHECK(!memcmp(wire,expected_des,32));
    CHECK(meiso_display_link_desired_decode((MeisoByteView){expected_des,32},&got));
    CHECK(meiso_display_link_desired_supported(&got_cap,&got));
    got.tier=1; CHECK(!meiso_display_link_desired_supported(&cap,&got));
    got=desired;got.downlink_kbit_s=1001;CHECK(!meiso_display_link_desired_supported(&cap,&got));
    got=desired;got.lease_ms[0]=1999;CHECK(!meiso_display_link_desired_encode(&got,(MeisoMutableBuffer){wire,40}));
    got.lease_ms[0]=5001;CHECK(!meiso_display_link_desired_encode(&got,(MeisoMutableBuffer){wire,40}));
    got=desired;got.keepalive_interval_ms=UINT32_MAX;got.keepalive_miss_limit=UINT32_MAX;
    CHECK(!meiso_display_link_desired_encode(&got,(MeisoMutableBuffer){wire,40})); /* no u32 wrap */
    CHECK(!meiso_display_link_desired_decode((MeisoByteView){expected_des,31},&got));
    got=desired;memcpy(wire,expected_des,32);wire[0]=3;
    CHECK(!meiso_display_link_desired_decode((MeisoByteView){wire,32},&got) && got.tier==0);
    memcpy(wire,expected_cap,40);wire[39]=1;
    CHECK(!meiso_display_link_capability_decode((MeisoByteView){wire,40},&got_cap));
    uint32_t reason=99;
    CHECK(meiso_display_link_terminal_encode(1,(MeisoMutableBuffer){wire,40}));
    CHECK(meiso_display_link_terminal_decode((MeisoByteView){wire,8},&reason) && reason==1);
    wire[4]=1;CHECK(!meiso_display_link_terminal_decode((MeisoByteView){wire,8},&reason) && reason==1);

    MeisoDisplayLinkHeartbeat heartbeat={.interval_ms=1000,.miss_limit=5};
    CHECK(meiso_display_link_heartbeat_due(&heartbeat,100));
    CHECK(meiso_display_link_heartbeat_sent(&heartbeat,1,100));
    CHECK(!meiso_display_link_heartbeat_due(&heartbeat,4000));
    CHECK(!meiso_display_link_heartbeat_sent(&heartbeat,2,4000));
    CHECK(!meiso_display_link_heartbeat_observe(&heartbeat,2));
    CHECK(!meiso_display_link_heartbeat_lost(&heartbeat,99));
    CHECK(!meiso_display_link_heartbeat_lost(&heartbeat,5099));
    CHECK(meiso_display_link_heartbeat_lost(&heartbeat,5100));
    CHECK(meiso_display_link_heartbeat_observe(&heartbeat,1));
    CHECK(!meiso_display_link_heartbeat_lost(&heartbeat,5100));
    CHECK(meiso_display_link_heartbeat_sent(&heartbeat,2,5100));
    CHECK(!meiso_display_link_heartbeat_observe(&heartbeat,1) && heartbeat.outstanding==2);
    CHECK(meiso_display_link_heartbeat_observe(&heartbeat,2));
    CHECK(!meiso_display_link_heartbeat_due(&heartbeat,6099));
    CHECK(meiso_display_link_heartbeat_due(&heartbeat,6100));

    MeisoDisplayResidenceLease leases[3]={{.lease_ms=3000},{.lease_ms=4000},{.lease_ms=0}};
    for (unsigned i=0;i<3;++i) CHECK(meiso_display_residence_accept(&leases[i],1,100));
    CHECK(!meiso_display_residence_accept(&leases[0],1,3000)); /* duplicate cannot extend */
    CHECK(!meiso_display_residence_expired(&leases[0],99));
    CHECK(!meiso_display_residence_expired(&leases[0],3099));
    CHECK(meiso_display_residence_expired(&leases[0],3100));
    CHECK(!meiso_display_residence_expired(&leases[1],3100));
    CHECK(!meiso_display_residence_expired(&leases[2],UINT64_MAX));
    leases[0].active=false; /* caller has now completed physical cleanup */
    for (unsigned i=0;i<3;++i) meiso_display_residence_renew(&leases[i],desired.lease_ms[i],3200);
    CHECK(!leases[0].active && leases[0].renewed_ms==100); /* heartbeat never resurrects */
    CHECK(!meiso_display_residence_expired(&leases[1],7199));
    CHECK(meiso_display_residence_expired(&leases[1],7200));
    CHECK(!meiso_display_residence_accept(&leases[0],1,7200));
    CHECK(meiso_display_residence_accept(&leases[0],2,7200));
    CHECK(leases[0].active && !meiso_display_residence_expired(&leases[0],10199));
    CHECK(meiso_display_residence_expired(&leases[0],10200));

    MeisoDisplayApplicationCatalogConfig c={0}, decoded;
    c.video=(MeisoDisplayIngressCatalogConfig){1,{1280,720,30000,131072,1024,1,
        MEISO_STREAM_DEADLINE_UNSUPPORTED,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR}};
    c.output_width=1920;c.output_height=1080;c.video_layout=1;c.link=cap;
    c.scene=(MeisoDisplaySceneCapability){1920,1080,1152000,48,10,16,16,8,1,16,1};
    c.camera=(MeisoCompositionMediaCapability){.source=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA,
        .pixel_formats=MEISO_COMPOSITION_MEDIA_PIXEL_MASK_NV12,.codecs=MEISO_COMPOSITION_MEDIA_CODEC_MASK_RAW,
        .min_width=320,.min_height=240,.max_width=320,.max_height=240,.max_fps_millihz=2000,
        .max_access_unit_bytes=115200,.max_fragment_payload_bytes=4096};
    uint8_t caps[188];MeisoRuntimeEndpointDescriptor ep[4];MeisoRuntimeV4RouteDescriptor rt[12];
    for (unsigned mask=0;mask<8;++mask) {
        c.overlay_enabled=(mask&1)!=0;c.camera_enabled=(mask&2)!=0;c.link_enabled=(mask&4)!=0;
        const size_t ec=1+c.overlay_enabled+c.camera_enabled+c.link_enabled;
        const size_t rc=4+3*(c.overlay_enabled+c.camera_enabled)+2*c.link_enabled;
        CHECK(meiso_display_application_build_catalog_for_version(&c,11,(MeisoMutableBuffer){caps,188},ep,4,rt,12));
        CHECK(meiso_display_application_validate_catalog_for_version(ep,ec,rt,rc,11,&decoded));
        CHECK(decoded.link_enabled==c.link_enabled && decoded.camera_enabled==c.camera_enabled);
        CHECK(!meiso_display_application_validate_catalog_for_version(ep,ec,rt,rc,10,&decoded));
        CHECK(rt[1].route.resource.schema.version==2);
        if(c.link_enabled) {
            CHECK(meiso_endpoint_id_equal(ep[ec-1].id,*meiso_display_link_endpoint_id()));
            CHECK(!meiso_display_application_build_catalog_for_version(&c,10,(MeisoMutableBuffer){caps,188},ep,4,rt,12));
        }
    }
    puts("v11 link wire, bounded heartbeat, independent leases and catalog vectors passed");
    return 0;
}
