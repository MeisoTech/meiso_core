#include <meiso/profiles/display_ingress/camera.h>
#include "display_ingress_profile_private.h"
#include <string.h>

static MeisoRuntimeV4RouteDescriptor camera_route(unsigned i, uint64_t incarnation)
{
    MeisoRuntimeV4RouteDescriptor r={0};
    r.route.endpoint_id=*meiso_composition_camera_media_endpoint();
    r.route.endpoint_incarnation=incarnation;
    r.route.resource=meiso_composition_camera_media_resources(NULL)[i];
    r.route.facet=i==0?MEISO_RUNTIME_FACET_DESIRED:i==1?MEISO_RUNTIME_FACET_ACTUAL:MEISO_RUNTIME_FACET_DATA;
    r.delivery=i==2?MEISO_RUNTIME_DELIVERY_EXTERNAL_MEDIA:MEISO_RUNTIME_DELIVERY_INLINE;
    return r;
}
bool meiso_display_camera_configure_session(MeisoSessionConfig *out)
{
    if (!out) return false;
    MeisoSessionConfig v=*out;
    if (meiso_display_ingress_configure_session_for_format(&v,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR)!=MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    v.profile.version=MEISO_DISPLAY_CAMERA_PROFILE_VERSION; v.limits.max_registry_entries=9;
    if (!meiso_session_memory_required(&v)) return false;
    *out=v; return true;
}
bool meiso_display_camera_configure_runtime(MeisoRuntimeV4Config *out, MeisoSession *session)
{
    if (!out || !session) return false;
    MeisoRuntimeV4Config v={0}; v.base.session=session;
    v.base.max_endpoints=2; v.base.max_endpoint_history=4; v.base.max_routes=7;
    v.base.max_capability_bytes=96; v.base.max_desired_profile_bytes_per_endpoint=32;
    v.base.max_actual_profile_bytes_per_endpoint=80; v.max_blob_transfer_bytes=1;
    if (!meiso_runtime_v4_memory_required(&v)) return false;
    *out=v; return true;
}
bool meiso_display_camera_build_catalog(const MeisoDisplayIngressCatalogConfig *video,
    const MeisoCompositionMediaCapability *camera, MeisoMutableBuffer storage,
    MeisoRuntimeEndpointDescriptor *ep, size_t ec, MeisoRuntimeV4RouteDescriptor *rt, size_t rc)
{
    if (!video || !camera || !ep || !rt || !storage.data || ec<2 || rc<7 || storage.capacity<96 ||
        camera->source!=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA ||
        video->capability.frame_format!=MEISO_DISPLAY_INGRESS_FRAME_H264_IDR) return false;
    const void *p[]={video,camera,storage.data,ep,rt};
    const size_t n[]={sizeof(*video),sizeof(*camera),96,2*sizeof(*ep),7*sizeof(*rt)};
    for(unsigned i=0;i<5;++i) for(unsigned j=i+1;j<5;++j)
        if(meiso_display_ingress_private_ranges_overlap(p[i],n[i],p[j],n[j])) return false;
    uint8_t cap[96]; size_t written=0;
    MeisoRuntimeEndpointDescriptor e[2]={0}; MeisoRuntimeV4RouteDescriptor r[7]={0};
    if(meiso_display_ingress_build_catalog(video,(MeisoMutableBuffer){cap,32},e,1,r,4)!=MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        meiso_composition_media_capability_encode(camera,(MeisoMutableBuffer){cap+32,64},&written)!=MEISO_COMPOSITION_PROFILE_OK) return false;
    e[0].capability.data=storage.data;
    e[1]=(MeisoRuntimeEndpointDescriptor){*meiso_composition_camera_media_endpoint(),video->endpoint_incarnation,
        *meiso_composition_media_capability_schema(),{storage.data+32,64}};
    for(unsigned i=0;i<3;++i) r[4+i]=camera_route(i,video->endpoint_incarnation);
    memcpy(storage.data,cap,96); memcpy(ep,e,sizeof(e)); memcpy(rt,r,sizeof(r)); return true;
}
bool meiso_display_camera_validate_catalog(const MeisoRuntimeEndpointDescriptor *ep, size_t ec,
    const MeisoRuntimeV4RouteDescriptor *rt, size_t rc, MeisoDisplayIngressCapability *video,
    MeisoCompositionMediaCapability *camera)
{
    if (!ep || !rt || !video || !camera || ec!=2 || rc!=7) return false;
    const void *p[]={ep,rt,video,camera,ep[0].capability.data,ep[1].capability.data};
    const size_t n[]={2*sizeof(*ep),7*sizeof(*rt),sizeof(*video),sizeof(*camera),ep[0].capability.size,ep[1].capability.size};
    for(unsigned i=2;i<4;++i) for(unsigned j=0;j<6;++j)
        if(i!=j && meiso_display_ingress_private_ranges_overlap(p[i],n[i],p[j],n[j])) return false;
    MeisoDisplayIngressCapability v; MeisoCompositionMediaCapability c;
    if(meiso_display_ingress_validate_catalog(ep,1,rt,4,&v)!=MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        v.frame_format!=MEISO_DISPLAY_INGRESS_FRAME_H264_IDR ||
        !meiso_endpoint_id_equal(ep[1].id,*meiso_composition_camera_media_endpoint()) ||
        ep[1].incarnation!=ep[0].incarnation ||
        !meiso_schema_ref_equal(ep[1].capability_schema,*meiso_composition_media_capability_schema()) ||
        meiso_composition_media_capability_decode(ep[1].capability,&c)!=MEISO_COMPOSITION_PROFILE_OK ||
        c.source!=MEISO_COMPOSITION_MEDIA_SOURCE_CAMERA) return false;
    for(unsigned i=0;i<3;++i) {
        const MeisoRuntimeV4RouteDescriptor e=camera_route(i,ep[1].incarnation), *r=&rt[4+i];
        if(!meiso_endpoint_id_equal(r->route.endpoint_id,e.route.endpoint_id) ||
            r->route.endpoint_incarnation!=e.route.endpoint_incarnation || r->route.facet!=e.route.facet ||
            r->delivery!=e.delivery || !meiso_resource_descriptor_equal(&r->route.resource,&e.route.resource)) return false;
    }
    *video=v; *camera=c; return true;
}
