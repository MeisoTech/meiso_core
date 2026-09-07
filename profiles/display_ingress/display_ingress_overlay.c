#include <meiso/profiles/display_ingress/overlay.h>
#include "display_ingress_profile_private.h"
#include <string.h>

#define put32 meiso_display_ingress_private_put_u32
#define put64 meiso_display_ingress_private_put_u64
#define get32 meiso_display_ingress_private_get_u32
#define get64 meiso_display_ingress_private_get_u64
#define zero meiso_display_ingress_private_bytes_zero
#define overlap meiso_display_ingress_private_ranges_overlap

static bool enc(const void *v, size_t vs, MeisoMutableBuffer out, size_t size)
{ return v && out.data && out.capacity >= size && !overlap(v,vs,out.data,size); }
static bool dec(MeisoByteView in, size_t size, void *v, size_t vs)
{ return v && in.data && in.size == size && !overlap(v,vs,in.data,in.size); }
static bool dimension(uint32_t n) { return n && n <= 4096; }
static bool cap_valid(const MeisoDisplayOverlayCapability *v)
{
    return v && dimension(v->output_width) && dimension(v->output_height) &&
        v->max_pixels && v->max_pixels <= MEISO_DISPLAY_OVERLAY_MAX_PIXELS &&
        v->max_text && v->max_text <= MEISO_DISPLAY_OVERLAY_MAX_TEXT &&
        v->max_updates_per_second && v->max_updates_per_second <= 60 &&
        dimension(v->min_width) && dimension(v->min_height) &&
        v->min_width<=v->output_width && v->min_height<=v->output_height &&
        dimension(v->width_alignment) && dimension(v->height_alignment) &&
        !(v->width_alignment & (v->width_alignment-1)) && !(v->height_alignment & (v->height_alignment-1)) &&
        v->min_width%v->width_alignment==0 && v->min_height%v->height_alignment==0 &&
        (uint32_t)v->min_width*v->min_height<=v->max_pixels;
}
static bool character(unsigned char c)
{ return (c>='A' && c<='Z') || (c>='0' && c<='9') || c==' ' || c=='-' || c=='.' || c==':' || c=='/'; }
static bool plan_valid(const MeisoDisplayOverlayPlan *v)
{
    if (!v || !v->generation || !v->video_desired_version || !dimension(v->canvas_width) ||
        !dimension(v->canvas_height) || v->visible > 1 || v->text_size > MEISO_DISPLAY_OVERLAY_MAX_TEXT)
        return false;
    if (!zero((const uint8_t *)v->text + v->text_size, sizeof(v->text)-v->text_size)) return false;
    for (unsigned i=0;i<v->text_size;++i) if (!character((unsigned char)v->text[i])) return false;
    if (!v->visible) return !v->x && !v->y && !v->width && !v->height && !v->border_argb &&
        !v->fill_argb && !v->text_argb && !v->border_width && !v->text_scale && !v->text_size;
    if (!dimension(v->width) || !dimension(v->height) || !v->text_scale || v->text_scale>8 ||
        v->border_width>v->width/2 || v->border_width>v->height/2) return false;
    return !v->text_size || ((uint32_t)(v->text_size*6-1)*v->text_scale <= v->width-2*v->border_width &&
        7U*v->text_scale <= v->height-2*v->border_width);
}
bool meiso_display_overlay_plan_supported(const MeisoDisplayOverlayCapability *c,
    const MeisoDisplayOverlayPlan *v, uint64_t video_version)
{
    return cap_valid(c) && plan_valid(v) && v->video_desired_version==video_version &&
        v->canvas_width==c->output_width && v->canvas_height==c->output_height &&
        v->text_size<=c->max_text && (!v->visible ||
        (v->width>=c->min_width && v->height>=c->min_height &&
         v->width%c->width_alignment==0 && v->height%c->height_alignment==0 &&
         v->width<=c->output_width && v->height<=c->output_height &&
         v->x<=c->output_width-v->width && v->y<=c->output_height-v->height &&
         (uint64_t)v->width*v->height<=c->max_pixels));
}
bool meiso_display_overlay_capability_encode(const MeisoDisplayOverlayCapability *v, MeisoMutableBuffer out)
{
    if (!enc(v,sizeof(*v),out,32) || !cap_valid(v)) return false;
    uint8_t b[32]={0}; put32(b,v->output_width); put32(b+4,v->output_height);
    put32(b+8,v->max_pixels); put32(b+12,v->max_text); put32(b+16,v->max_updates_per_second);
    const uint16_t limits[]={v->min_width,v->min_height,v->width_alignment,v->height_alignment};
    for(unsigned i=0;i<4;++i) { b[20+2*i]=(uint8_t)(limits[i]>>8); b[21+2*i]=(uint8_t)limits[i]; }
    memcpy(out.data,b,32); return true;
}
bool meiso_display_overlay_capability_decode(MeisoByteView in, MeisoDisplayOverlayCapability *out)
{
    if (!dec(in,32,out,sizeof(*out)) || !zero(in.data+28,4)) return false;
    MeisoDisplayOverlayCapability v={get32(in.data),get32(in.data+4),get32(in.data+8),get32(in.data+12),get32(in.data+16),0,0,0,0};
    v.min_width=(uint16_t)((uint16_t)in.data[20]<<8|in.data[21]);
    v.min_height=(uint16_t)((uint16_t)in.data[22]<<8|in.data[23]);
    v.width_alignment=(uint16_t)((uint16_t)in.data[24]<<8|in.data[25]);
    v.height_alignment=(uint16_t)((uint16_t)in.data[26]<<8|in.data[27]);
    if (!cap_valid(&v)) return false;
    *out=v; return true;
}
bool meiso_display_overlay_plan_encode(const MeisoDisplayOverlayPlan *v, MeisoMutableBuffer out)
{
    if (!enc(v,sizeof(*v),out,112) || !plan_valid(v)) return false;
    uint8_t b[112]={0}; put64(b,v->generation); put64(b+8,v->video_desired_version);
    put32(b+16,v->canvas_width); put32(b+20,v->canvas_height); put32(b+24,v->x); put32(b+28,v->y);
    put32(b+32,v->width); put32(b+36,v->height); put32(b+40,v->border_argb);
    put32(b+44,v->fill_argb); put32(b+48,v->text_argb);
    b[52]=(uint8_t)(v->border_width>>8); b[53]=(uint8_t)v->border_width;
    b[54]=v->text_scale; b[55]=v->visible; b[56]=v->text_size;
    memcpy(b+64,v->text,v->text_size); memcpy(out.data,b,112); return true;
}
bool meiso_display_overlay_plan_decode(MeisoByteView in, MeisoDisplayOverlayPlan *out)
{
    if (!dec(in,112,out,sizeof(*out)) || !zero(in.data+57,7)) return false;
    const uint8_t *b=in.data; MeisoDisplayOverlayPlan v={0};
    v.generation=get64(b); v.video_desired_version=get64(b+8);
    v.canvas_width=get32(b+16); v.canvas_height=get32(b+20); v.x=get32(b+24); v.y=get32(b+28);
    v.width=get32(b+32); v.height=get32(b+36); v.border_argb=get32(b+40);
    v.fill_argb=get32(b+44); v.text_argb=get32(b+48);
    v.border_width=(uint16_t)((uint16_t)b[52]<<8 | b[53]); v.text_scale=b[54]; v.visible=b[55]; v.text_size=b[56];
    memcpy(v.text,b+64,sizeof(v.text)); if (!plan_valid(&v)) return false;
    *out=v; return true;
}
bool meiso_display_overlay_evidence_encode(const MeisoDisplayOverlayEvidence *v, MeisoMutableBuffer out)
{
    if (!enc(v,sizeof(*v),out,56) || !v->plan_generation || !v->desired_version) return false;
    uint8_t b[56]={2}; put64(b+8,v->plan_generation); put64(b+16,v->desired_version);
    memcpy(b+24,v->plan_sha256,32); memcpy(out.data,b,56); return true;
}
bool meiso_display_overlay_evidence_decode(MeisoByteView in, MeisoDisplayOverlayEvidence *out)
{
    if (!dec(in,56,out,sizeof(*out)) || in.data[0]!=2 || !zero(in.data+1,7)) return false;
    MeisoDisplayOverlayEvidence v={0}; v.plan_generation=get64(in.data+8); v.desired_version=get64(in.data+16);
    if (!v.plan_generation || !v.desired_version) return false;
    memcpy(v.plan_sha256,in.data+24,32); *out=v; return true;
}

static const MeisoEndpointId endpoint={UINT64_C(0x444953504c41595f),UINT64_C(0x4855445f454e445f)};
static const MeisoSchemaRef capability_schema={{UINT64_C(0x444953505f534348),UINT64_C(0x4855445f4341505f)},1};
static const uint64_t suffix[3]={UINT64_C(0x4855445f504c414e),UINT64_C(0x4855445f53544154),UINT64_C(0x4855445f50524553)};
static MeisoRuntimeV4RouteDescriptor hud_route(unsigned i, uint64_t incarnation)
{
    MeisoRuntimeV4RouteDescriptor v={0}; v.route.endpoint_id=endpoint; v.route.endpoint_incarnation=incarnation;
    v.route.resource.id=(MeisoResourceId){UINT64_C(0x444953504c41595f),suffix[i]};
    v.route.resource.schema=(MeisoSchemaRef){{UINT64_C(0x444953505f534348),suffix[i]},1};
    v.route.resource.kind=i==2?MEISO_RESOURCE_KIND_STREAM:MEISO_RESOURCE_KIND_STATE;
    v.route.resource.owner=i==0?MEISO_OWNER_ROLE_HOST:MEISO_OWNER_ROLE_EDGE;
    v.route.resource.max_payload_size=i==0?MEISO_RUNTIME_DESIRED_STATE_PREFIX_SIZE+112:
        i==1?MEISO_RUNTIME_ACTUAL_STATE_PREFIX_SIZE+8:56;
    v.route.facet=i==0?MEISO_RUNTIME_FACET_DESIRED:i==1?MEISO_RUNTIME_FACET_ACTUAL:MEISO_RUNTIME_FACET_DATA;
    v.delivery=MEISO_RUNTIME_DELIVERY_INLINE; return v;
}
bool meiso_display_overlay_configure_session(MeisoSessionConfig *out)
{
    if (!out) return false;
    MeisoSessionConfig v=*out;
    if (meiso_display_ingress_configure_session_for_format(&v,MEISO_DISPLAY_INGRESS_FRAME_H264_IDR)!=MEISO_DISPLAY_INGRESS_PROFILE_OK) return false;
    v.profile.version=3; v.limits.max_registry_entries=9;
    if (!meiso_session_memory_required(&v)) return false;
    *out=v; return true;
}
bool meiso_display_overlay_configure_runtime(MeisoRuntimeV4Config *out, MeisoSession *session)
{
    if (!out || !session) return false;
    MeisoRuntimeV4Config v={0}; v.base.session=session;
    v.base.max_endpoints=2; v.base.max_endpoint_history=4; v.base.max_routes=7;
    v.base.max_capability_bytes=64; v.base.max_desired_profile_bytes_per_endpoint=112;
    v.base.max_actual_profile_bytes_per_endpoint=32; v.max_blob_transfer_bytes=1;
    if (!meiso_runtime_v4_memory_required(&v)) return false;
    *out=v; return true;
}
bool meiso_display_overlay_build_catalog(const MeisoDisplayIngressCatalogConfig *video,
    const MeisoDisplayOverlayCapability *hud, MeisoMutableBuffer storage,
    MeisoRuntimeEndpointDescriptor *ep, size_t ec, MeisoRuntimeV4RouteDescriptor *rt, size_t rc)
{
    if (!video || !hud || !ep || !rt || !storage.data || ec<2 || rc<7 || storage.capacity<64 ||
        video->capability.frame_format!=MEISO_DISPLAY_INGRESS_FRAME_H264_IDR) return false;
    const void *p[]={video,hud,storage.data,ep,rt};
    const size_t n[]={sizeof(*video),sizeof(*hud),64,2*sizeof(*ep),7*sizeof(*rt)};
    for(unsigned i=0;i<5;++i) for(unsigned j=i+1;j<5;++j) if(overlap(p[i],n[i],p[j],n[j])) return false;
    uint8_t cap[64]; MeisoRuntimeEndpointDescriptor e[2]={0}; MeisoRuntimeV4RouteDescriptor r[7]={0};
    if (meiso_display_ingress_build_catalog(video,(MeisoMutableBuffer){cap,32},e,1,r,4)!=MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        !meiso_display_overlay_capability_encode(hud,(MeisoMutableBuffer){cap+32,32})) return false;
    e[0].capability.data=storage.data;
    e[1]=(MeisoRuntimeEndpointDescriptor){endpoint,video->endpoint_incarnation,capability_schema,{storage.data+32,32}};
    for(unsigned i=0;i<3;++i) r[4+i]=hud_route(i,video->endpoint_incarnation);
    memcpy(storage.data,cap,64); memcpy(ep,e,sizeof(e)); memcpy(rt,r,sizeof(r)); return true;
}
bool meiso_display_overlay_validate_catalog(const MeisoRuntimeEndpointDescriptor *ep, size_t ec,
    const MeisoRuntimeV4RouteDescriptor *rt, size_t rc, MeisoDisplayIngressCapability *video, MeisoDisplayOverlayCapability *hud)
{
    if (!ep || !rt || !video || !hud || ec!=2 || rc!=7 ||
        overlap(video,sizeof(*video),hud,sizeof(*hud))) return false;
    for(unsigned i=0;i<2;++i) {
        if(overlap(video,sizeof(*video),ep[i].capability.data,ep[i].capability.size) ||
            overlap(hud,sizeof(*hud),ep[i].capability.data,ep[i].capability.size)) return false;
    }
    if(overlap(video,sizeof(*video),ep,2*sizeof(*ep)) || overlap(hud,sizeof(*hud),ep,2*sizeof(*ep)) ||
        overlap(video,sizeof(*video),rt,7*sizeof(*rt)) || overlap(hud,sizeof(*hud),rt,7*sizeof(*rt))) return false;
    MeisoDisplayIngressCapability v; MeisoDisplayOverlayCapability h;
    if(meiso_display_ingress_validate_catalog(ep,1,rt,4,&v)!=MEISO_DISPLAY_INGRESS_PROFILE_OK ||
        v.frame_format!=MEISO_DISPLAY_INGRESS_FRAME_H264_IDR ||
        !meiso_endpoint_id_equal(ep[1].id,endpoint) || ep[1].incarnation!=ep[0].incarnation ||
        !meiso_schema_ref_equal(ep[1].capability_schema,capability_schema) ||
        !meiso_display_overlay_capability_decode(ep[1].capability,&h)) return false;
    for(unsigned i=0;i<3;++i) {
        const MeisoRuntimeV4RouteDescriptor expected=hud_route(i,ep[1].incarnation), *r=&rt[4+i];
        if(!meiso_endpoint_id_equal(r->route.endpoint_id,expected.route.endpoint_id) ||
            r->route.endpoint_incarnation!=expected.route.endpoint_incarnation || r->route.facet!=expected.route.facet ||
            r->delivery!=expected.delivery || !meiso_resource_descriptor_equal(&r->route.resource,&expected.route.resource)) return false;
    }
    *video=v; *hud=h; return true;
}
