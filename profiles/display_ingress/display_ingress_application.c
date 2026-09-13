#include <meiso/profiles/display_ingress/application.h>
#include "display_ingress_profile_private.h"
#include <string.h>
#define overlap meiso_display_ingress_private_ranges_overlap
#define get32 meiso_display_ingress_private_get_u32
#define put32 meiso_display_ingress_private_put_u32
static const MeisoSchemaRef schema={{UINT64_C(0x444953505f534348),UINT64_C(0x4341504142494c5f)},3};
const MeisoSchemaRef *meiso_display_application_video_capability_schema(void){return &schema;}
static bool geometry(const MeisoDisplayApplicationCatalogConfig *c)
{
    return c->video.capability.frame_format==MEISO_DISPLAY_INGRESS_FRAME_H264_IDR &&
        (c->video.capability.evidence_classes==1 || c->video.capability.evidence_classes==2) &&
        c->output_width && c->output_width<=4096 && c->output_height && c->output_height<=4096 &&
        c->video.capability.max_input_width<=c->output_width && c->video.capability.max_input_height<=c->output_height &&
        c->video_layout<=1 && (!c->video_layout || !(c->output_width%2 || c->video.capability.max_input_width%2)) &&
        (!c->overlay_enabled || (c->overlay.output_width==c->output_width && c->overlay.output_height==c->output_height));
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
    if(!geometry(&c)) return false;
    *video=c.video.capability;*w=c.output_width;*h=c.output_height;*layout=c.video_layout;return true;
}
bool meiso_display_application_configure_session(MeisoSessionConfig *out)
{
    if(!out) return false;
    MeisoSessionConfig c=*out;
    if(meiso_display_ingress_configure_session_for_format(&c,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR)!=MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    c.profile.version=9;c.limits.max_registry_entries=13;
    if(!meiso_session_memory_required(&c)) return false;
    *out=c;return true;
}
bool meiso_display_application_configure_runtime(MeisoRuntimeV4Config *out,MeisoSession *session)
{
    if(!out || !session) return false;
    MeisoRuntimeV4Config c={0};c.base.session=session;c.base.max_endpoints=3;c.base.max_endpoint_history=6;
    c.base.max_routes=10;c.base.max_capability_bytes=148;c.base.max_desired_profile_bytes_per_endpoint=112;
    c.base.max_actual_profile_bytes_per_endpoint=80;c.max_blob_transfer_bytes=1;
    if(!meiso_runtime_v4_memory_required(&c)) return false;
    *out=c;return true;
}
bool meiso_display_application_build_catalog(const MeisoDisplayApplicationCatalogConfig *c,
    MeisoMutableBuffer storage,MeisoRuntimeEndpointDescriptor *ep,size_t ec,MeisoRuntimeV4RouteDescriptor *rt,size_t rc)
{
    if(!c || !geometry(c) || !ep || !rt || !storage.data) return false;
    const size_t endpoints=1+c->overlay_enabled+c->camera_enabled,routes=4+3*(endpoints-1);
    const size_t bytes=48+36*c->overlay_enabled+64*c->camera_enabled;
    if(ec<endpoints || rc<routes || storage.capacity<bytes) return false;
    const void *p[]={c,ep,rt,storage.data};const size_t n[]={sizeof(*c),endpoints*sizeof(*ep),routes*sizeof(*rt),bytes};
    for(unsigned i=0;i<4;++i) for(unsigned j=i+1;j<4;++j) if(overlap(p[i],n[i],p[j],n[j])) return false;
    uint8_t cap[148]={0},tmp[96];MeisoRuntimeEndpointDescriptor e[3]={0},te[2];MeisoRuntimeV4RouteDescriptor r[10]={0},tr[7];
    if(meiso_display_ingress_build_catalog(&c->video,(MeisoMutableBuffer){cap,32},e,1,r,4)!=MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    e[0].capability_schema=schema;e[0].capability=(MeisoByteView){storage.data,48};
    put32(cap+32,c->output_width);put32(cap+36,c->output_height);cap[40]=c->video_layout;
    size_t at=48,ei=1,ri=4;
    if(c->overlay_enabled){
        if(!meiso_display_overlay_build_catalog(&c->video,&c->overlay,(MeisoMutableBuffer){tmp,sizeof(tmp)},te,2,tr,7)) return false;
        memcpy(cap+at,tmp+32,36);e[ei]=te[1];e[ei++].capability=(MeisoByteView){storage.data+at,36};at+=36;
        memcpy(r+ri,tr+4,3*sizeof(*r));r[ri+2].route.resource.schema.version=2;ri+=3;
    }
    if(c->camera_enabled){
        if(!meiso_display_camera_build_catalog(&c->video,&c->camera,(MeisoMutableBuffer){tmp,sizeof(tmp)},te,2,tr,7)) return false;
        memcpy(cap+at,tmp+32,64);e[ei]=te[1];e[ei].capability=(MeisoByteView){storage.data+at,64};
        memcpy(r+ri,tr+4,3*sizeof(*r));
    }
    memcpy(storage.data,cap,bytes);memcpy(ep,e,endpoints*sizeof(*ep));memcpy(rt,r,routes*sizeof(*rt));return true;
}
bool meiso_display_application_validate_catalog(const MeisoRuntimeEndpointDescriptor *ep,size_t ec,
    const MeisoRuntimeV4RouteDescriptor *rt,size_t rc,MeisoDisplayApplicationCatalogConfig *out)
{
    if(!ep || !rt || !out || ec<1 || ec>3 || rc!=4+3*(ec-1) ||
        overlap(out,sizeof(*out),ep,ec*sizeof(*ep)) || overlap(out,sizeof(*out),rt,rc*sizeof(*rt))) return false;
    for(size_t i=0;i<ec;++i) if(overlap(out,sizeof(*out),ep[i].capability.data,ep[i].capability.size)) return false;
    MeisoDisplayApplicationCatalogConfig c={0};c.video.endpoint_incarnation=ep[0].incarnation;
    if(!meiso_schema_ref_equal(ep[0].capability_schema,schema) ||
        !meiso_display_application_video_capability_decode(ep[0].capability,&c.video.capability,&c.output_width,&c.output_height,&c.video_layout)) return false;
    size_t ei=1;
    if(ei<ec && !meiso_endpoint_id_equal(ep[ei].id,*meiso_composition_camera_media_endpoint())){
        c.overlay_enabled=true;
        if(!meiso_display_overlay_capability_decode(ep[ei++].capability,&c.overlay)) return false;
    }
    if(ei<ec){
        c.camera_enabled=true;
        if(meiso_composition_media_capability_decode(ep[ei++].capability,&c.camera)!=MEISO_COMPOSITION_PROFILE_OK) return false;
    }
    if(ei!=ec) return false;
    uint8_t cap[148];MeisoRuntimeEndpointDescriptor e[3];MeisoRuntimeV4RouteDescriptor r[10];
    if(!meiso_display_application_build_catalog(&c,(MeisoMutableBuffer){cap,sizeof(cap)},e,3,r,10)) return false;
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
