#include <meiso/profiles/display_ingress/application.h>
#include "display_ingress_profile_private.h"
#include <string.h>
#define overlap meiso_display_ingress_private_ranges_overlap
#define get32 meiso_display_ingress_private_get_u32
#define put32 meiso_display_ingress_private_put_u32
static const MeisoSchemaRef schema={{UINT64_C(0x444953505f534348),UINT64_C(0x4341504142494c5f)},3};
const MeisoSchemaRef *meiso_display_application_video_capability_schema(void){return &schema;}
MeisoDisplayIngressProfileStatus meiso_display_application_validate_video_actual(uint32_t version,
    const MeisoDisplayIngressCapability *capability, const MeisoRuntimeDesiredState *desired,
    const MeisoRuntimeActualState *state, MeisoDisplayIngressActualStateView *out)
{
    if(version==9 || version==10 || (version==11 && state && state->production_status==MEISO_RUNTIME_PRODUCTION_RUNNING))
        return meiso_display_ingress_validate_actual_state(capability,desired,state,out);
    if(version!=11 || !state || !out || !capability ||
        overlap(out,sizeof(*out),state,sizeof(*state)) || overlap(out,sizeof(*out),state->profile.data,state->profile.size) ||
        overlap(out,sizeof(*out),capability,sizeof(*capability)) || (desired &&
        (overlap(out,sizeof(*out),desired,sizeof(*desired)) || overlap(out,sizeof(*out),desired->profile.data,desired->profile.size))))
        return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_ARGUMENT;
    uint32_t reason;
    if(!meiso_display_link_terminal_decode(state->profile,&reason) || (reason==MEISO_DISPLAY_LINK_LEASE_EXPIRED &&
        ((state->production_status!=MEISO_RUNTIME_PRODUCTION_STOPPED && state->production_status!=MEISO_RUNTIME_PRODUCTION_FAILED) || !state->applied_desired.present ||
         !desired || !desired->enabled))) return MEISO_DISPLAY_INGRESS_PROFILE_INVALID_VALUE;
    MeisoRuntimeActualState legacy=*state;legacy.profile=(MeisoByteView){NULL,0};
    return meiso_display_ingress_validate_actual_state(capability,desired,&legacy,out);
}
static bool geometry(const MeisoDisplayApplicationCatalogConfig *c,uint32_t version)
{
    return (version==9 || version==10 || version==11) && (!c->link_enabled || version==11) &&
        c->video.capability.frame_format==MEISO_DISPLAY_INGRESS_FRAME_H264_IDR &&
        (c->video.capability.evidence_classes==1 || c->video.capability.evidence_classes==2) &&
        c->output_width && c->output_width<=4096 && c->output_height && c->output_height<=4096 &&
        c->video.capability.max_input_width<=c->output_width && c->video.capability.max_input_height<=c->output_height &&
        c->video_layout<=1 && (!c->video_layout || !(c->output_width%2 || c->video.capability.max_input_width%2)) &&
        (!c->overlay_enabled || (version==9 ?
            c->overlay.output_width==c->output_width && c->overlay.output_height==c->output_height :
            c->scene.output_width==c->output_width && c->scene.output_height==c->output_height));
}
bool meiso_display_application_video_capability_decode(MeisoByteView in,
    MeisoDisplayIngressCapability *video,uint32_t *w,uint32_t *h,uint8_t *layout)
{
    if(!video || !w || !h || !layout || !in.data || in.size!=48) return false;
    const void *p[]={video,w,h,layout,in.data};const size_t n[]={sizeof(*video),sizeof(*w),sizeof(*h),1,48};
    for(unsigned i=0;i<4;++i) for(unsigned j=i+1;j<5;++j) if(overlap(p[i],n[i],p[j],n[j])) return false;
    MeisoDisplayApplicationCatalogConfig c={0};
    if(meiso_display_ingress_capability_decode((MeisoByteView){in.data,32},&c.video.capability)!=MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_display_ingress_private_bytes_zero(in.data+41,7)) return false;
    c.output_width=get32(in.data+32);c.output_height=get32(in.data+36);c.video_layout=in.data[40];
    if(!geometry(&c,9)) return false;
    *video=c.video.capability;*w=c.output_width;*h=c.output_height;*layout=c.video_layout;return true;
}
bool meiso_display_application_configure_session_for_version(MeisoSessionConfig *out,uint32_t version)
{
    if(!out || (version!=9 && version!=10 && version!=11)) return false;
    MeisoSessionConfig c=*out;
    if(meiso_display_ingress_configure_session_for_format(&c,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR)!=MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    c.profile.version=version;c.limits.max_registry_entries=version==11?16:13;
    if(!meiso_session_memory_required(&c)) return false;
    *out=c;return true;
}
bool meiso_display_application_configure_runtime_for_version(MeisoRuntimeV4Config *out,MeisoSession *session,uint32_t version)
{
    if(!out || !session || (version!=9 && version!=10 && version!=11)) return false;
    MeisoRuntimeV4Config c={0};c.base.session=session;c.base.max_endpoints=version==11?4:3;c.base.max_endpoint_history=version==11?8:6;
    c.base.max_routes=version==11?12:10;c.base.max_capability_bytes=version==11?188:148;
    c.base.max_desired_profile_bytes_per_endpoint=version>=10?MEISO_DISPLAY_SCENE_MAX_PLAN_BYTES:112;
    c.base.max_actual_profile_bytes_per_endpoint=80;c.max_blob_transfer_bytes=1;
    if(!meiso_runtime_v4_memory_required(&c)) return false;
    *out=c;return true;
}
bool meiso_display_application_build_catalog_for_version(const MeisoDisplayApplicationCatalogConfig *c,uint32_t version,
    MeisoMutableBuffer storage,MeisoRuntimeEndpointDescriptor *ep,size_t ec,MeisoRuntimeV4RouteDescriptor *rt,size_t rc)
{
    if(!c || !geometry(c,version) || !ep || !rt || !storage.data) return false;
    const size_t endpoints=1+c->overlay_enabled+c->camera_enabled+c->link_enabled;
    const size_t routes=4+3*(c->overlay_enabled+c->camera_enabled)+2*c->link_enabled;
    const size_t bytes=48+36*c->overlay_enabled+64*c->camera_enabled+40*c->link_enabled;
    if(ec<endpoints || rc<routes || storage.capacity<bytes) return false;
    const void *p[]={c,ep,rt,storage.data};const size_t n[]={sizeof(*c),endpoints*sizeof(*ep),routes*sizeof(*rt),bytes};
    for(unsigned i=0;i<4;++i) for(unsigned j=i+1;j<4;++j) if(overlap(p[i],n[i],p[j],n[j])) return false;
    uint8_t cap[188]={0},tmp[96];MeisoRuntimeEndpointDescriptor e[4]={0},te[2];MeisoRuntimeV4RouteDescriptor r[12]={0},tr[7];
    if(meiso_display_ingress_build_catalog(&c->video,(MeisoMutableBuffer){cap,32},e,1,r,4)!=MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    e[0].capability_schema=schema;e[0].capability=(MeisoByteView){storage.data,48};
    put32(cap+32,c->output_width);put32(cap+36,c->output_height);cap[40]=c->video_layout;
    if(version==11) r[MEISO_DISPLAY_INGRESS_ROUTE_ACTUAL].route.resource.schema.version=2;
    size_t at=48,ei=1,ri=4;
    if(c->overlay_enabled){
        const bool scene=version>=10;
        if(scene ? !meiso_display_scene_capability_encode(&c->scene,(MeisoMutableBuffer){cap+at,36}) :
            !meiso_display_overlay_capability_encode(&c->overlay,(MeisoMutableBuffer){cap+at,36})) return false;
        e[ei]=meiso_display_overlay_private_endpoint(c->video.endpoint_incarnation);
        e[ei].capability_schema.version=scene?4:3;e[ei++].capability=(MeisoByteView){storage.data+at,36};at+=36;
        for(unsigned i=0;i<3;++i) r[ri+i]=meiso_display_overlay_private_route(i,c->video.endpoint_incarnation);
        if(scene) { r[ri].route.resource.schema.version=4;
            r[ri].route.resource.max_payload_size=MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE+MEISO_DISPLAY_SCENE_MAX_PLAN_BYTES; }
        if(version==11) { r[ri+1].route.resource.schema.version=2;
            r[ri+1].route.resource.max_payload_size=MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE+16; }
        r[ri+2].route.resource.schema.version=2;ri+=3;
    }
    if(c->camera_enabled){
        if(!meiso_display_camera_build_catalog(&c->video,&c->camera,(MeisoMutableBuffer){tmp,sizeof(tmp)},te,2,tr,7)) return false;
        memcpy(cap+at,tmp+32,64);e[ei]=te[1];e[ei].capability=(MeisoByteView){storage.data+at,64};
        memcpy(r+ri,tr+4,3*sizeof(*r));
        if(version==11) r[ri+1].route.resource.schema.version=2;
        ++ei;ri+=3;at+=64;
    }
    if(c->link_enabled){
        if(!meiso_display_link_capability_encode(&c->link,(MeisoMutableBuffer){cap+at,40})) return false;
        e[ei]=meiso_display_link_endpoint(c->video.endpoint_incarnation);
        e[ei].capability=(MeisoByteView){storage.data+at,40};
        for(unsigned i=0;i<2;++i) r[ri+i]=meiso_display_link_route(i,c->video.endpoint_incarnation);
    }
    memcpy(storage.data,cap,bytes);memcpy(ep,e,endpoints*sizeof(*ep));memcpy(rt,r,routes*sizeof(*rt));return true;
}
bool meiso_display_application_validate_catalog_for_version(const MeisoRuntimeEndpointDescriptor *ep,size_t ec,
    const MeisoRuntimeV4RouteDescriptor *rt,size_t rc,uint32_t version,MeisoDisplayApplicationCatalogConfig *out)
{
    if((version!=9 && version!=10 && version!=11) || !ep || !rt || !out || ec<1 || ec>(version==11?4U:3U) || rc<4 || rc>12 ||
        overlap(out,sizeof(*out),ep,ec*sizeof(*ep)) || overlap(out,sizeof(*out),rt,rc*sizeof(*rt))) return false;
    for(size_t i=0;i<ec;++i) if(overlap(out,sizeof(*out),ep[i].capability.data,ep[i].capability.size)) return false;
    MeisoDisplayApplicationCatalogConfig c={0};c.video.endpoint_incarnation=ep[0].incarnation;
    if(!meiso_schema_ref_equal(ep[0].capability_schema,schema) ||
        !meiso_display_application_video_capability_decode(ep[0].capability,&c.video.capability,&c.output_width,&c.output_height,&c.video_layout)) return false;
    size_t ei=1;
    if(ei<ec && !meiso_endpoint_id_equal(ep[ei].id,*meiso_composition_camera_media_endpoint()) &&
        !meiso_endpoint_id_equal(ep[ei].id,*meiso_display_link_endpoint_id())){
        c.overlay_enabled=true;
        if(version>=10 ? !meiso_display_scene_capability_decode(ep[ei++].capability,&c.scene) :
            !meiso_display_overlay_capability_decode(ep[ei++].capability,&c.overlay)) return false;
    }
    if(ei<ec && meiso_endpoint_id_equal(ep[ei].id,*meiso_composition_camera_media_endpoint())){
        c.camera_enabled=true;
        if(meiso_composition_media_capability_decode(ep[ei++].capability,&c.camera)!=MEISO_COMPOSITION_PROFILE_OK) return false;
    }
    if(ei<ec && version==11 && meiso_endpoint_id_equal(ep[ei].id,*meiso_display_link_endpoint_id())){
        c.link_enabled=true;
        if(!meiso_display_link_capability_decode(ep[ei++].capability,&c.link)) return false;
    }
    if(ei!=ec || rc!=4+3*(c.overlay_enabled+c.camera_enabled)+2*c.link_enabled) return false;
    uint8_t cap[188];MeisoRuntimeEndpointDescriptor e[4];MeisoRuntimeV4RouteDescriptor r[12];
    if(!meiso_display_application_build_catalog_for_version(&c,version,(MeisoMutableBuffer){cap,sizeof(cap)},e,4,r,12)) return false;
    for(size_t i=0;i<ec;++i) if(!meiso_endpoint_id_equal(ep[i].id,e[i].id) || ep[i].incarnation!=e[i].incarnation ||
        !meiso_schema_ref_equal(ep[i].capability_schema,e[i].capability_schema) || ep[i].capability.size!=e[i].capability.size ||
        !ep[i].capability.data || memcmp(ep[i].capability.data,e[i].capability.data,e[i].capability.size)) return false;
    for(size_t i=0;i<rc;++i) if(!meiso_endpoint_id_equal(rt[i].route.endpoint_id,r[i].route.endpoint_id) ||
        rt[i].route.endpoint_incarnation!=r[i].route.endpoint_incarnation || rt[i].route.facet!=r[i].route.facet ||
        rt[i].delivery!=r[i].delivery || !meiso_resource_descriptor_equal(&rt[i].route.resource,&r[i].route.resource)) return false;
    *out=c;return true;
}
bool meiso_display_overlay_evidence_encode_for_class(const MeisoDisplayOverlayEvidence *v,
    MeisoDisplayIngressEvidenceClass cls,MeisoMutableBuffer out)
{
    if((cls!=1 && cls!=2) || !meiso_display_overlay_evidence_encode(v,out)) return false;
    out.data[0]=cls;return true;
}
bool meiso_display_overlay_evidence_decode_for_class(MeisoByteView in,
    MeisoDisplayIngressEvidenceClass cls,MeisoDisplayOverlayEvidence *out)
{
    if((cls!=1 && cls!=2) || !in.data || in.size!=56 || in.data[0]!=cls || !out ||
        overlap(in.data,in.size,out,sizeof(*out))) return false;
    uint8_t b[56];memcpy(b,in.data,56);b[0]=2;
    return meiso_display_overlay_evidence_decode((MeisoByteView){b,56},out);
}

bool meiso_display_application_configure_session(MeisoSessionConfig *c)
{ return meiso_display_application_configure_session_for_version(c,9); }
bool meiso_display_application_configure_runtime(MeisoRuntimeV4Config *c,MeisoSession *s)
{ return meiso_display_application_configure_runtime_for_version(c,s,9); }
bool meiso_display_application_build_catalog(const MeisoDisplayApplicationCatalogConfig *c,
    MeisoMutableBuffer b,MeisoRuntimeEndpointDescriptor *e,size_t ec,MeisoRuntimeV4RouteDescriptor *r,size_t rc)
{ return meiso_display_application_build_catalog_for_version(c,9,b,e,ec,r,rc); }
bool meiso_display_application_validate_catalog(const MeisoRuntimeEndpointDescriptor *e,size_t ec,
    const MeisoRuntimeV4RouteDescriptor *r,size_t rc,MeisoDisplayApplicationCatalogConfig *c)
{ return meiso_display_application_validate_catalog_for_version(e,ec,r,rc,9,c); }
